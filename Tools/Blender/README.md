# Blender Tools

## generate_british_infantry.py

Proceduralny generator modelu piechocinca Brytyjskiego (era napoleońska).

### Jak użyć

1. Otwórz **Blender** (3.x lub nowszy)
2. Przejdź do zakładki **Scripting**
3. Kliknij **Open** i wybierz ten plik, lub wklej zawartość do edytora
4. Kliknij **Run Script** (Shift+Enter)
5. Model `BP_BritishInfantry` pojawi się w scenie

### Eksport do UE5

`File → Export → FBX (.fbx)`

Ustawienia eksportu:
- **Scale**: 1.0
- **Apply Unit**: tak
- **Forward**: -Y Forward
- **Up**: Z Up

W UE5 przy imporcie:
- **Combine Meshes**: tak
- **Import as Skeletal**: nie (statyczny mesh dla HISM)
- **Generate Missing Collision**: tak

### Co zawiera model

| Część | Materiał |
|-------|---------|
| Tors + rękawy | Scarlet red coat |
| Spodnie | Białe/szare |
| Buty / kamaszetki | Czarne |
| Głowa / dłonie | Skóra |
| Shako (czako) | Czarny z mosiężną plakietą |
| Krzyżowe pasy | Białe skórzane |
| Muszkiet Brown Bess + bagnet | Metal + drewno |

### Docelowa liczba polygonów

~800–1200 poly — zoptymalizowane pod setki jednostek w UE5 HISM/Mass Entity.
