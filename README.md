# tcp 16-20 checker (cli)
🚀 This repository contains checkers that allow you to determine if your “home” ISP has DPI, as well as the specific methods (and their parameters) the censor uses for limitations.

> [!NOTE] 
> Most of the code in the repository is written by AI. The code may be imperfect and not ideal.

> [!WARNING]  
> All content in this repository is provided **for research and educational purposes only**.  
> You are **solely responsible** for ensuring that your use of any code, data, or information from this repository complies with all applicable laws and regulations in your jurisdiction.  
> The authors and contributors **assume no liability** for any misuse or violations arising from the use of this materials.

### build
```bash
g++ -std=c++23 dpi.cpp -lcurl -pthread -O2 -o dpi_check
```

### usage
```bash
./dpi_check [timeout_ms]
```

See [here](https://github.com/net4people/bbs/issues/490) for details on this blocking method.

The original repository is available [here](https://github.com/hyperion-cs/dpi-checkers).
