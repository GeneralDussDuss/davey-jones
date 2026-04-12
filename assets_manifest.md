# DAVEY JONES — Asset Manifest

Curated imagery, fonts, and palettes for the branding of the firmware fork. All entries vetted for license (no Disney POTC likenesses, no watermarks, no unclear AI provenance).

---

## 1. Reference Imagery (mood/inspiration only — not for direct use)

1. **Unsplash — "Deep Sea" collection** — https://unsplash.com/s/photos/deep-sea
   Abyssal blues, light rays piercing dark water. Core mood baseline.

2. **Unsplash — Bioluminescence tag** — https://unsplash.com/s/photos/bioluminescence
   Acid-green plankton glow. The "thing in the deep" color language.

3. **Wikimedia — NOAA Okeanos Explorer deep-sea gallery** — https://commons.wikimedia.org/wiki/Category:Photographs_by_NOAA_Okeanos_Explorer
   Public-domain abyssal creatures, siphonophores, anglerfish.

4. **Wikimedia — pre-1928 kraken illustrations** — https://commons.wikimedia.org/wiki/Category:Kraken
   PD engravings of tentacles breaching ships. Folklore-pure, zero Disney.

5. **Wikimedia — Pierre Denys de Montfort "Poulpe Colossal" (1801)** — https://commons.wikimedia.org/wiki/File:Colossal_octopus_by_Pierre_Denys_de_Montfort.jpg
   **THE kraken illustration. PD. Silhouette reads even at 16px.** Hero splash source.

6. **ArtStation — "abyssal horror"** — https://www.artstation.com/search?sort_by=relevance&query=abyssal%20horror
   Concept art for shape language. Reference only.

7. **Pinterest — "vaporwave ocean"** — https://www.pinterest.com/search/pins/?q=vaporwave%20ocean
   Lisa-Frank-meets-Lovecraft intersection.

8. **Flickr CC-search — "diving helmet"** — https://www.flickr.com/search/?text=diving%20helmet&license=2%2C3%2C4%2C5%2C6%2C9
   Barnacle-crusted standard-dress helmets.

9. **OpenGameArt — "underwater" tag** — https://opengameart.org/art-search-advanced?keys=underwater
   Pixel-art refs at LCD-native resolutions.

10. **itch.io — "cosmic horror" free asset packs** — https://itch.io/game-assets/free/tag-cosmic-horror
    Check each license individually.

---

## 2. Directly-Usable Assets

### Repo banner / README header
- **NOAA — "Deep-sea coral community, Gulf of Mexico"** — PD
  https://commons.wikimedia.org/wiki/File:Expl0936_-_Flickr_-_NOAA_Photo_Library.jpg
  Bioluminescent reds against abyssal black. Crop to 1200×400, tint teal.

- **NOAA Okeanos — "Medusa siphonophore, Gulf of Mexico"** — PD
  https://commons.wikimedia.org/wiki/File:Siphonophore_(25744796218).jpg
  Alien bioluminescent chain on pure black. Perfect banner aspect.

### App icon candidates
- **Kraken by Andrejs Kirma** — CC-BY 3.0 (credit required)
  https://thenounproject.com/icon/kraken-1885321/
  Clean silhouette, reads at 16×16 and scales to hero.

- **Diving Helmet by Gan Khoon Lay** — CC-BY 3.0 (credit required)
  https://thenounproject.com/icon/diving-helmet-1142301/
  Non-pirate, works monochrome.

- **OpenMoji — Octopus (U+1F419)** — CC-BY-SA 4.0
  https://openmoji.org/library/emoji-1F419/
  SVG source, black variant included.

### Splash screen (135×240 LCD)
- **Denys de Montfort kraken** — PD — **PRIMARY PICK** (see ref #5)
  High-contrast silhouette, trivially reduces to RGB565. Ship at bottom, tentacles wrapping frame.

- **OpenGameArt — "Lovecraftian 1-bit assets"** — filter for CC0
  https://opengameart.org/content/lovecraftian-1bit-assets
  Native pixel res, 1-bit → RGB565 with no dithering.

- **Ernst Haeckel "Kunstformen der Natur" (1904)** — PD
  https://commons.wikimedia.org/wiki/Category:Kunstformen_der_Natur
  Plate 86 (Discomedusae), Plate 18 (Siphonophorae) — already high-contrast stylized.

### Repo ornament / dividers
- **Haeckel plates** (same link above) — PD
  Drop between README sections as horizontal rules.

---

## 3. Color Palettes

### Palette A — "Abyssal Neon" (hero/primary — web + README)
```
#0B0E1A  midnight hull
#0E4C5C  deep teal
#16E0BD  bioluminescent cyan
#C724B1  tentacle magenta
#F5F5F5  bone white
```

### Palette B — "Drowned Vapor" (trippy lean — splash)
```
#1A0B2E  void purple
#2E1A47  bruise
#6B2FB3  eldritch violet
#00F5D4  plankton glow
#FEE440  lantern-bait yellow
```

### Palette C — "Barnacle & Brine" (muted — dial-back option)
```
#0D1B1E  black water
#1F3A3D  weathered bronze-teal
#84A98C  sea lichen
#CAD2C5  foam
#E63946  warning red
```

### Palette D — "LCD-Safe RGB565 High-Contrast" (FIRMWARE UI PICK)
```
#000000  pure black     (0x0000)
#00FFFF  cyan           (0x07FF)
#FF00FF  magenta        (0xF81F)
#FFFF00  yellow         (0xFFE0)
#FFFFFF  white          (0xFFFF)
```
Maps exactly to RGB565 primaries — zero quantization loss, readable in direct sunlight on a 1.14" panel.
Plan: black bg, cyan body text, magenta accents, yellow reserved for alerts/warnings.

### Palette E — "Haeckel Plate" (vintage engraving mode — README headers)
```
#F4E9D8  aged paper
#3C2A1E  sepia ink
#5C1A1B  dried blood
#1C3D4A  deep indigo
```

---

## 4. Font Picks

### Firmware LCD UI
- **Monogram** by datagoblin — **CC0** — **PICK**
  https://datagoblin.itch.io/monogram
  5×7 pixel monospace, no attribution, no-brainer default.

- **Pixellari** — free for commercial
  https://www.dafont.com/pixellari.font
  Chunkier alternative for splash titles.

- **VCR OSD Mono** — free for commercial
  https://www.dafont.com/vcr-osd-mono.font
  CRT/VHS vibe for HUD overlays.

### README / web headers
- **Nosifer** (OFL) — https://fonts.google.com/specimen/Nosifer
  Dripping horror display. **Wordmark only** — unreadable at body size.

- **Rubik Glitch** (OFL) — https://fonts.google.com/specimen/Rubik+Glitch
  Digital-corruption look.

- **Monoton** (OFL) — https://fonts.google.com/specimen/Monoton
  Retro-neon marquee.

### Pairing (confirmed stack)
Wordmark: **Nosifer** → Subhead: **Monoton** → Body: **JetBrains Mono** (OFL, existing pentesting community default).

---

## License Hygiene Checklist

1. Noun Project icons → attribution in README footer (e.g. "Kraken icon by Andrejs Kirma from the Noun Project (CC-BY 3.0)").
2. OpenMoji → "Emoji artwork from OpenMoji (CC-BY-SA 4.0)". SA clause: any modifications must ship under CC-BY-SA — fine for README, think twice before baking into closed derivatives.
3. NOAA / Haeckel / Denys de Montfort → PD, no credit required, but include a goodwill acknowledgment.
4. Fonts → drop license files into `assets/fonts/LICENSES/` alongside the `.ttf`/`.otf`.
5. OpenGameArt entries → verify per-asset license before commit. Site aggregates CC0 / CC-BY / CC-BY-SA / GPL mixed.

---

## Hero Picks (TL;DR for next design pass)

| Slot | Asset | License |
|---|---|---|
| **Splash** (135×240) | Denys de Montfort "Poulpe Colossal" 1801 kraken → pixel-art reduction | PD |
| **App icon** (16px & up) | Noun Project "Kraken" by Andrejs Kirma | CC-BY 3.0 |
| **Repo banner** | NOAA Okeanos siphonophore photo → teal tint + wordmark overlay | PD |
| **Firmware palette** | Palette D (RGB565-safe black/cyan/magenta/yellow/white) | — |
| **Web palette** | Palette A "Abyssal Neon" | — |
| **LCD font** | Monogram 5×7 | CC0 |
| **Wordmark font** | Nosifer (hero only), Monoton (subhead), JetBrains Mono (body) | OFL |
