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

### Liczba polygonów

Skrypt jest obecnie w trybie **high-detail** — używa gęstych siatek + zaaplikowanego
modyfikatora **Subdivision Surface (poziom 2)** dla organicznych/ubraniowych części
(obłe, naturalne kształty + Shade Smooth). Polycount NIE jest zoptymalizowany.

Dodatkowe detale w tej wersji:
- Poły munduru (coat tails) opadające na uda + boczne panele
- Wyłogi (lapels) z rzędem mosiężnych guzików + stojący kołnierz
- Mankiety (cuffs) i pagony/naramienniki na barkach
- Sferyczne stawy (bark, łokieć, biodro, kolano) dla płynnych zgięć po subdiv
- Głowa z zarysowaną żuchwą/podbródkiem (nie sama sfera)
- Czako z daszkiem (peak), false-front, plakietą i pomponem
- Pasy krzyżowe rzutowane na krzywiznę klatki piersiowej (przylegają)

**Przed masowym instancjonowaniem (HISM)** zdecymuj mesh i zbuduj LOD-y.
