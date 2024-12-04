# Fingerprint_extractor
Minutae based feature extraction from fingerprint image. Used fingerjet engine. It's only for non commercial uses and learning purpose. https://github.com/FingerJetFXOSE/FingerJetFXOSE

## Build instruction
Run the following command to build

```
g++ -g -Wall src/fp_extractor_example.cpp -o dist/output -lpthread -lm -lc -ldl -I/home/mehmet/c_projects/fp-extractor/include -L/home/mehmet/c_projects/fp-extractor/lib -lFJFX -lFRFXLL
```
