# Fingerprint Feature Extractor (Minutiae-Based)
This repository contains the source code for a Fingerprint Feature Extraction tool written in C. The tool uses a minutiae-based approach, adhering to the MINEX (Minutiae Exchange) standard for fingerprint feature extraction.
Minutae based feature extraction from fingerprint image. Used fingerjet engine. It's only for non commercial uses and learning purpose. https://github.com/FingerJetFXOSE/FingerJetFXOSE

## Features
- Minutiae-based fingerprint feature extraction.
- Compliance with MINEX standards for interoperability.
- Optimized performance for large-scale fingerprint datasets.
- Configurable parameters for minutiae detection.

## Build instruction
Run the following command to build

```
g++ -g -Wall src/fp_extractor_example.cpp -o dist/output -lpthread -lm -lc -ldl -I/home/mehmet/c_projects/fp-extractor/include -L/home/mehmet/c_projects/fp-extractor/lib -lFJFX -lFRFXLL
```
