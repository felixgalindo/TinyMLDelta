# Contributing to TinyMLDelta

Thank you for your interest in contributing to **TinyMLDelta** — an incremental model-update system for TinyML and embedded AI devices.

TinyMLDelta aims to build the missing *model-infrastructure layer* for Edge AI: safe updates, guardrails, metadata, and OTA-friendly deltas for ML models running on microcontrollers.

Contributions of all kinds are welcome — code, docs, integrations, examples, bug reports, and feature proposals.

---

## 🧭 How to Contribute

### 1. Fork the Repository
Click **Fork** at the top right of this repo and clone your fork:

```bash
git clone https://github.com/<yourname>/TinyMLDelta.git
cd TinyMLDelta
```

### 2. Create a Feature Branch
```bash
git checkout -b feature/my-improvement
```

### 3. Make Your Changes
Follow the directory structure:

- **cli/** — PatchGen, metadata extraction, Python tooling  
- **runtime/** — C core: patch application engine  
- **examples/** — POSIX flash simulation + model generator  

### 4. Run the Tests / Demo
Before submitting, ensure the POSIX demo works:

```bash
cd cli
./install.sh
source .tinyenv/bin/activate

cd ../examples/posix
./run_demo.sh
```

This validates patch generation, flash creation, patch application, and verification.

### 5. Submit a Pull Request
Push your branch and open a PR:

```bash
git push origin feature/my-improvement
```

Include a clear description of:

- What you changed  
- Why the change is needed  
- Any limitations / follow-ups  

---

## 📝 Contribution Areas

TinyMLDelta is early but rapidly growing. You can contribute to:

### 🔧 Core Features & Algorithms
- Improved diff algorithms  
- Signed patch support (SHA-256, AES-CMAC, COSE)  
- Metadata TLV extensions  
- Reduced RAM footprint  
- Extended journaling  

### 🖥 MCU Integrations
- Zephyr  
- Arduino UNO R4 WiFi  
- Particle Tachyon  
- STM32 / NRF52 / ESP32  

### 🧪 Testing
- POSIX expansion  
- Fuzz testing  
- Corrupted patch handling  
- Multi-chunk stress tests  

### 🐍 Python Tooling
- Edge Impulse frontend  
- Auto-metadata improvements  
- Better logging + JSON outputs  

### 📚 Documentation
- Tutorials  
- TLV explanations  
- OTA integration examples  

---

## 🧩 Coding Style

### C Runtime
- Keep core logic platform-agnostic  
- Avoid heap allocations where possible  
- Use the existing ports structure  

### Python Tools
- Python ≥ 3.9  
- Minimal dependencies  
- Prefer clarity + type hints  

---

## 🐛 Reporting Bugs

Please include:

- Platform (macOS, Linux, MCU target)  
- Steps to reproduce  
- Patch file (if possible)  
- Flash dump (if applicable)  
- Logs  

Submit issues at:  
https://github.com/felixgalindo/TinyMLDelta/issues

---

## 🌱 Roadmap (High-Level)

- Edge Impulse support  
- Signing + certificate blocks  
- Model lineage/versioning  
- OTA pipelines  
- MCU integrations  

---

## 🤝 Code of Conduct

Be professional and constructive.  
TinyMLDelta aims to be a welcoming space for embedded AI developers.

---

## 🙏 Thank You!

Your contributions help build the **edge-AI model infrastructure layer** the industry desperately needs.
