# NLX-REPO
## server implementation

### system requirements
 - linux
 - cpu (optional)
 - g++-13+
 - cxx_std_23
 - internet access

### build
```bash
cmake -B build -DCMAKE_CXX_COMPILER=/usr/bin/g++-13 -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build -j$(nproc)
```

### usage 
default configuration from `src/core/config.cpp`
```bash
./build/bin/supervisor
```

custom configuration from `data/default.cfg`
```bash
./build/bin/supervisor ./data/default.cfg
```

### tools
cpu cores isolation:
 ```bash
sudo sh ./scripts/isolate_cpu.sh
```

update ca bundle:
```bash
sh ./scripts/update_certs.sh ./certs/cacert.pem 
```
