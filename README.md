# NLX-REPO
## server implementation

### requirements
 - linux
 - cpu (optional)
 - gcc11+
 - cxx20

### build
```bash
cmake .
make
```

### usage 
 - default configuration from `src/core/config.cpp`
```bash
./bin/supervisor
```

 - custom configuration from `data/default.cfg`
```bash
./bin/supervisor ./data/default.cfg
```

### tools
 - cpu isolation:
 ```bash
sudo sh ./scripts/cpu_isolate.sh 
```
