{
  "targets": [
    {
      "target_name": "binding",
      "sources": [
        "binding.cc",
        "memcache.cc",
        "bson.cc"
      ],
      "include_dirs" : [
        "<!(node -e \"require('nan')\")"
      ],
      "link_settings": {
		"libraries": [
		  "-lrt"
		]
	  }
    }
  ]}
