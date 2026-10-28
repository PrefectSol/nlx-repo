# NLX-REPO

## system requirements
 - linux
 - cpu (optional)
 - g++-13+
 - cxx_std_23
 - internet access

## build
```bash
cmake -B build -DCMAKE_CXX_COMPILER=/usr/bin/g++-13 -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build -j$(nproc)
```

## usage 
```bash
./build/bin/supervisor
```

## tools
 - cpu cores isolation:
 ```bash
sudo bash ./scripts/isolate_cpu.sh
```

 - update ca bundle:
```bash
bash ./scripts/update_certs.sh ./certs/cacert.pem
```
