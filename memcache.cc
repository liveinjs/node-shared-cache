#include<stdio.h>
#include<string.h>
#include<errno.h>
#include "lock.h"
#include "memcache.h"

#define    MAGIC_NUM    0xdeadbeef
#define    BLK_SIZE     1024

namespace cache {

typedef struct node_slave_s {
    uint32_t    slave_next;
    uint8_t        data[0];
} node_slave_t;

typedef struct node_s {
    uint32_t    slave_next;
    uint32_t    prev;
    uint32_t    next;
    uint32_t    hash_next;
    uint32_t    blocks;
    uint32_t    valLen;
    uint32_t    hash;
    uint16_t    keyLen;
    uint16_t    key[0];
} node_t;

typedef struct cache_s {
    union {
        struct {
            uint32_t    magic;
            uint32_t    size;
            uint32_t    blocks_used;
            uint32_t    blocks_total; // maximum block count = 65536*32-513 = 2096639
            uint32_t    head;
            uint32_t    tail;
            uint32_t    nextBit; // next block to look for when allocating block

			rw_lock_t   lock;
        } info; // assert(sizeof(cache.info) < 64)

        // bitmap[0~15] is not used
        uint32_t bitmap[65536]; // maximum memory size available is 65536*32*1K = 2G
    };
    uint32_t hashmap[65536];
    
} cache_t;

void init(void* ptr, size_t size) {
    cache_t& cache = *static_cast<cache_t*>(ptr);
    if(cache.info.magic == MAGIC_NUM && cache.info.size == size) { // already initialized
        // fprintf(stderr, "init cache: already initialized\n");
        return;
    }
    memset(&cache, 0, sizeof(cache_t));
    // fprintf(stderr, "sizeof(cache_t):%d==524288 sizeof(cache.info):%d<64\n", sizeof(cache_t), sizeof(cache.info));

    // Use forced write lock to prevent deadlock caused by a crashed thread
    write_lock_t lock(cache.info.lock);
    
    cache.info.magic = MAGIC_NUM;
    cache.info.size = size;
    
    uint32_t blocks = size >> 10;
    cache.info.blocks_total = blocks - 512;
    cache.info.blocks_used = 0;
    cache.info.nextBit = 16;
    // fprintf(stderr, "init cache: size %d, blocks %d, usage %d/%d\n", size, blocks, cache.info.blocks_used, cache.info.blocks_total);
}

static uint32_t selectOne(cache_t& cache) {
    uint32_t& curr = cache.info.nextBit;
    uint32_t bits = cache.bitmap[curr];
    while(bits == 0xffffffff) {
        curr++;
        if(curr == (cache.info.blocks_total + 513) >> 5) {
            curr = 16;
        }
        bits = cache.bitmap[curr];
    }
    // assert(bits != 0xffffffff)
    uint32_t bitSelected = 31 - __builtin_clz(~bits);
    cache.bitmap[curr] = bits | 1 << bitSelected;
    return curr << 5 | bitSelected;
}

template<typename T>
inline T* address(void* base, uint32_t block) {
    return reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(base) + (block << 10));
}

static void dump(cache_t& cache) {
    fprintf(stderr, "== DUMP START: head: %d tail:%d", cache.info.head, cache.info.tail);
    uint32_t prev = 0;

    for(uint32_t curr = cache.info.head; curr;) {
        node_t& node = *address<node_t>(&cache, curr);
        if(node.prev != prev) {
            fprintf(stderr, "ERROR: %d->prev=%d != %d\n", curr, node.prev, prev);
        }
        fprintf(stderr, "\n%d(prev:%d next:%d)", curr, node.prev, node.next);
        for(uint32_t slave_next = node.slave_next; slave_next; ) {
            node_slave_t& slave = *address<node_slave_t>(&cache, slave_next);
            fprintf(stderr, "-->%d", slave_next);
            slave_next = slave.slave_next;
        }
        prev = curr;
        curr = node.next;
    }
    if(prev != cache.info.tail) {
        fprintf(stderr, "\nERROR: ended at %d != tail(%d)\n", prev, cache.info.tail);
    } else {
        fprintf(stderr, "\nDUMP END ==\n");
    }

}

inline void touch(cache_t& cache, uint32_t curr) {
    // fprintf(stderr, "cache::touch %d (current: %d)\n", curr, cache.info.tail);
    if(cache.info.tail == curr) {
        return;
    }
     // bring to tail
    node_t& node = *address<node_t>(&cache, curr);
    // assert: node.next != 0
    address<node_t>(&cache, node.next)->prev = node.prev;

    if(node.prev) {
        address<node_t>(&cache, node.prev)->next = node.next;
    } else { // node is head
        cache.info.head = node.next;
    }
    node.next = 0;
    node.prev = cache.info.tail;
    address<node_t>(&cache, cache.info.tail)->next = curr;
    cache.info.tail = curr;
    // fprintf(stderr, "touch: head=%d tail=%d curr=%d prev=%d next=%d\n", cache.info.head, cache.info.tail, curr, node.prev, node.next);
}

inline uint32_t hashsum(const uint16_t* key, size_t keyLen) {
    uint32_t hash = 0xffffffff;
    for(size_t i = 0; i < keyLen; i++) {
        hash = hash * 31 + key[i];
    }
    return hash;
}

inline uint32_t find(cache_t& cache, const uint16_t* key, size_t keyLen) {
    uint32_t hash = hashsum(key, keyLen);
    
    uint32_t curr = cache.hashmap[hash & 0xffff];
    // fprintf(stderr, "cache::find: finding match key (len:%d)\n", keyLen);
    while(curr) {
        node_t& node = *address<node_t>(&cache, curr);
        if(node.keyLen == keyLen && node.hash == hash && !memcmp(node.key, key, keyLen << 1)) {
            // fprintf(stderr, "cache::find: found match block %d (keyLen=%d hash=%d)\n", curr, keyLen, hash);
            return curr;
        }
        curr = node.hash_next;
    }

    return 0;
}

inline void release(cache_t& cache, uint32_t block) {
    uint32_t count = 0;
    for(uint32_t next = block; next; next = address<node_slave_t>(&cache, next)->slave_next) {
        // fprintf(stderr, "free block %d assert(bit:%d) %x ^ %x = %x\n", next, (cache.bitmap[next >> 5] >> (next & 31)) & 1, cache.bitmap[next >> 5], 1 << (next & 31), cache.bitmap[next >> 5] ^ 1 << (next & 31));
        cache.bitmap[next >> 5] ^= 1 << (next & 31);
        // fprintf(stderr, "freed 1 bit,  bitmap[%d] = %x\n", next>>5, cache.bitmap[next>>5]);
        count++;
    }
    cache.info.blocks_used -= count;
}

inline void dropNode(cache_t& cache, uint32_t firstBlock) {
    node_t& node = *address<node_t>(&cache, firstBlock);
    // fprintf(stderr, "dropping node %d (prev:%d next:%d)\n", firstBlock, node.prev, node.next);
    // remove from lru list
    uint32_t& $prev = node.prev ? address<node_t>(&cache, node.prev)->next : cache.info.head;
    uint32_t& $next = node.next ? address<node_t>(&cache, node.next)->prev : cache.info.tail;
    $prev = node.next;
    $next = node.prev;

    // remove from hash list
    uint32_t* toModify = &cache.hashmap[node.hash & 0xffff];
    while(*toModify != firstBlock) {
        node_t* pcurr = address<node_t>(&cache, *toModify);
        toModify = &pcurr->hash_next;
    }
    *toModify = node.hash_next;
    // release blocks
    release(cache, firstBlock);
}

inline uint32_t allocate(cache_t& cache, uint32_t count) {
    uint32_t target = cache.info.blocks_total - count;
    // fprintf(stderr, "allocate: used=%d, count=%d, target=%d\n", cache.info.blocks_used, count, target);
    if(cache.info.blocks_used > target) { // not enough
        do {
            // fprintf(stderr, "allocate: need to drop %d more blocks (head=%d)\n", cache.info.blocks_used - target, cache.info.head);
            dropNode(cache, cache.info.head);
        } while (cache.info.blocks_used > target);
    }
    // TODO
    uint32_t firstBlock = selectOne(cache);
    // fprintf(stderr, "select %d blocks (first: %d)\n", count, firstBlock);
    cache.info.blocks_used += count;

    node_slave_t* node = address<node_slave_t>(&cache, firstBlock);
    while(--count) {
        uint32_t nextBlock = selectOne(cache);
        // fprintf(stderr, "selected block  %d\n", nextBlock);
        node->slave_next = nextBlock;
        node = address<node_slave_t>(&cache, nextBlock);
    }
    // close last slave
    node->slave_next = 0;
    return firstBlock;
}

inline void slave_next(cache_t& cache, node_slave_t*& node, uint32_t n) {
    for(uint32_t i = 1; i < n; i++) {
        node = address<node_slave_t>(&cache, node->slave_next);
    }
}

void get(void* ptr, const uint16_t* key, size_t keyLen, uint8_t*& retval, size_t& retvalLen) {
    // fprintf(stderr, "cache::get: key len %d\n", keyLen);
    cache_t& cache = *static_cast<cache_t*>(ptr);

    read_lock_t lock(cache.info.lock);
    uint32_t found = find(cache, key, keyLen);
    if(found) {
        touch(cache, found);
        node_t* pnode = address<node_t>(&cache, found);
        size_t  valLen = retvalLen = pnode->valLen;
        uint8_t* val = retval = new uint8_t[valLen];

        node_slave_t* currentBlock = reinterpret_cast<node_slave_t*>(pnode);
        uint32_t offset = sizeof(node_t) + (keyLen << 1);
        uint32_t capacity = BLK_SIZE - offset;

        while(capacity < valLen) {
            //// fprintf(stderr, "copying val (%x+%d) %d bytes\n", currentBlock, offset, capacity);
            memcpy(val, reinterpret_cast<uint8_t*>(currentBlock) + offset, capacity);
            val += capacity;
            valLen -= capacity;
            currentBlock = address<node_slave_t>(&cache, currentBlock->slave_next);
            offset = sizeof(node_slave_t);
            capacity = BLK_SIZE - sizeof(node_slave_t);
        }
        if(valLen) { // capacity >= valLen
            // fprintf(stderr, "copying remaining val (%x+%d) %d bytes\n", currentBlock, offset, valLen);
            memcpy(val, reinterpret_cast<uint8_t*>(currentBlock) + offset, valLen);
        }
    } else {
        retval = NULL;
    }

    // dump(cache);
}

int set(void* ptr, const uint16_t* key, size_t keyLen, const uint8_t* val, size_t valLen) {
    size_t totalLen = keyLen + valLen + sizeof(node_s) - 1;
    uint32_t blocksRequired = totalLen / 1023 + (totalLen % 1023 ? 1 : 0);
    // fprintf(stderr, "cache::set: total len %d (%d blocks required)\n", totalLen, blocksRequired);

    cache_t& cache = *static_cast<cache_t*>(ptr);

    if(blocksRequired > cache.info.blocks_total) {
        errno = E2BIG;
        return -1;
    }

    write_lock_t lock(cache.info.lock);

    // find if key is already exists
    uint32_t found = find(cache, key, keyLen);
    node_t* selectedBlock;

    if(found) { // update
        touch(cache, found);
        selectedBlock = address<node_t>(&cache, found);
        node_t& node = *selectedBlock;
        if(node.blocks > blocksRequired) { // free extra blocks
            node_slave_t* pcurr = reinterpret_cast<node_slave_t*>(&node);
            slave_next(cache, pcurr, blocksRequired);
            // drop remaining blocks
            release(cache, pcurr->slave_next);
            // fprintf(stderr, "freeing %d blocks (%d used)\n", node.blocks - blocksRequired, cache.info.blocks_used);
            pcurr->slave_next = 0;
        } else if(node.blocks < blocksRequired) {
            node_slave_t* pcurr = reinterpret_cast<node_slave_t*>(&node);
            slave_next(cache, pcurr, node.blocks);
            // allocate more blocks
            // fprintf(stderr, "acquiring extra %d blocks assert(last:%d == 0) (%d used)\n", blocksRequired - node.blocks, pcurr->slave_next, cache.info.blocks_used);
            pcurr->slave_next = allocate(cache, blocksRequired - node.blocks);
        }
        node.blocks = blocksRequired;
    } else { // insert
        // insert into hash table
        uint32_t firstBlock = allocate(cache, blocksRequired);
        selectedBlock = address<node_t>(&cache, firstBlock);
        node_t& node = *selectedBlock;
        node.blocks = blocksRequired;

        node.hash = hashsum(key, keyLen);
        uint32_t& hash_head = cache.hashmap[node.hash & 0xffff];
        node.hash_next = hash_head; // insert into linked list
        hash_head = firstBlock;
        node.keyLen = keyLen;

        if(!cache.info.tail) {
            cache.info.head = cache.info.tail = firstBlock;
            node.prev = node.next = 0;
        } else {
            address<node_t>(&cache, cache.info.tail)->next = firstBlock;
            node.prev = cache.info.tail;
            node.next = 0;
            cache.info.tail = firstBlock;
        }
        // copy key
        // fprintf(stderr, "copying key (len:%d)\n", keyLen);
        memcpy(selectedBlock->key, key, keyLen << 1);
    }
    selectedBlock->valLen = valLen;

    // keyLen <= 256 < capacity
    // while(capacity < keyLen) {
    //     if(!found) memcpy(reinterpret_cast<uint8_t*>(currentBlock) + offset, key, capacity << 1);
    //     key += capacity;
    //     keyLen -= capacity;
    //     currentBlock = address<node_slave_t>(&cache, currentBlock->slave_next);
    //     offset = sizeof(node_slave_t);
    //     capacity = (BLK_SIZE - sizeof(node_slave_t)) >> 1;
    // }
    // if(keyLen) { // capacity >= keyLen
    // }

    // copy values
    node_slave_t* currentBlock = reinterpret_cast<node_slave_t*>(selectedBlock);
    uint32_t offset = sizeof(node_t) + (keyLen << 1);
    uint32_t capacity = BLK_SIZE - offset;

    while(capacity < valLen) {
        // fprintf(stderr, "copying val (%x+%d) %d bytes\n", currentBlock, offset, capacity);
        memcpy(reinterpret_cast<uint8_t*>(currentBlock) + offset, val, capacity);
        val += capacity;
        valLen -= capacity;
        currentBlock = address<node_slave_t>(&cache, currentBlock->slave_next);
        offset = sizeof(node_slave_t);
        capacity = BLK_SIZE - sizeof(node_slave_t);
    }

    if(valLen) { // capacity >= valLen
        // fprintf(stderr, "copying remaining val (%x+%d) %d bytes\n", currentBlock, offset, valLen);
        memcpy(reinterpret_cast<uint8_t*>(currentBlock) + offset, val, valLen);
    }
    // dump(cache);
    return 0;
}


void enumerate(void* ptr, EnumerateCallback& enumerator) {
    cache_t& cache = *static_cast<cache_t*>(ptr);
    read_lock_t lock(cache.info.lock);
    uint32_t curr = cache.info.head;

    while(curr) {
        node_t& node = *address<node_t>(&cache, curr);
        enumerator.next(node.key, node.keyLen);
        curr = node.next;
    }
}

bool contains(void* ptr, const uint16_t* key, size_t keyLen) {
    cache_t& cache = *static_cast<cache_t*>(ptr);

    read_lock_t lock(cache.info.lock);
    return find(cache, key, keyLen);
}

bool unset(void* ptr, const uint16_t* key, size_t keyLen) {
    cache_t& cache = *static_cast<cache_t*>(ptr);

    write_lock_t lock(cache.info.lock);
    uint32_t found = find(cache, key, keyLen);
    if(found) {
        dropNode(cache, found);
    }
    return found;
}

}
