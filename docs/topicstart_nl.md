# freetoon-lvgl — een complete, open vervanging voor de Toon-interface

*Topicstart — laatst bijgewerkt: 21 mei 2026*

## Wat is dit?

**freetoon-lvgl** is een volledig nieuwe, opensource gebruikersinterface voor de
(geroote) Eneco/Quby **Toon** en **Toon 2**. Het is een complete vervanging van
de originele `qt-gui` — diezelfde schil die je normaal op je Toon ziet, en die
berucht is om zijn geheugenlekken. freetoon is gebouwd met **LVGL** (een lichte
C/embedded UI-library) en draait een fractie zo zwaar als de originele schil.

Belangrijk: freetoon vervangt **alleen de schil**. De onderliggende logica van de
Toon (thermostaat, ketelaansturing/OpenTherm, meteradapter, ventilatie, Z-Wave,
enz.) blijft gewoon de stock-software van Quby. freetoon praat daar netjes mee via
de interne BoxTalk-bus en de lokale HTTP-API's. Je kunt dus altijd terug naar de
originele interface.

> **Beta.** Alle releases staan als *prerelease* gemarkeerd. Het werkt hier
> stabiel op twee Toons, maar gebruik het op eigen risico. Een geroote Toon is
> vereist (SSH-toegang).

---

## Installatie & updaten

Eén commando op de Toon (via SSH):

```
curl -fsSL https://raw.githubusercontent.com/Ierlandfan/freetoon-lvgl/main/scripts/toon-selfinstall.sh | sh
reboot
```

Het script haalt de nieuwste release op, zet het nieuwe `toonui`-binary + de
hulpscripts neer, bewaart een back-up (`toonui.bak`) en opent de benodigde
firewallpoorten. Updaten doe je door precies hetzelfde commando opnieuw te
draaien — of via de **About**-tegel in de UI (er verschijnt een banner op het
beginscherm zodra er een nieuwe release is, met *Installeer nu* / *Overslaan*).

### Terug naar de originele Toon-interface

Bij het opstarten verschijnt een **boot picker** met de keuze *freetoon-lvgl*
vs *Stock qt-gui* (10 sec, default freetoon). Je kunt ook in
**Instellingen → UI mode** permanent terugschakelen naar de stock-schil. Je raakt
dus nooit "vast".

---

## Het beginscherm

- **Grote thermostaattegel** linksboven: actuele temperatuur, setpoint met +/–,
  programma-knoppen (Comfort / Home / Sleep / Away) en Manual/Program-schakelaar.
  Onderaan: luchtvochtigheid, eCO₂, TVOC en keteldruk.
- **Tegels rechts** (Waste / Energy / Vent / Family / Water). Deze zijn
  **veegbaar**: veeg naar links voor een tweede pagina met 4 vrij in te delen
  tegels; de paginastipjes onderin tonen op welke pagina je zit.
- **Lichtknop** links in het midden: een klein "halve-cirkel"-lipje dat zich bij
  aanraken uitklapt tot een "Lights"-knop. Opent de lampenpagina (Home Assistant
  of Domoticz, afhankelijk van wat je hebt aangezet). Werkt ook met een
  veeg-naar-rechts op de thermostaathelft.
- **Nieuws-ticker** boven de weersverwachting: scrollende RSS-koppen. Tik erop
  voor de lijst met koppen + een **QR-code per artikel** om het op je telefoon te
  openen.
- **Weersverwachting** onderaan: meerdaagse strip met icoon, min/max en wind,
  gevoed door Buienradar. Tik voor de detailpagina.
- **Gordijnen-balk** (indien Home Assistant cover aanwezig): open/stop/dicht +
  positie en batterij.
- **Pakketten** (zie PWA hieronder).

---

## Instellingen (tandwiel rechtsboven, of via de telefoon)

Elke tegel opent een venster. Tekstvelden hebben een **schermtoetsenbord**, zodat
alles ook via VNC of touch in te vullen is.

| Tegel | Wat je instelt |
|---|---|
| **Display** | Auto-dim, dim-timeout, schermhelderheid (actief/dim), temperatuur-offset, weer/afval op het dim-scherm |
| **Weather** | Stad (zoekt automatisch het Buienradar-id op), forecast-modus (auto/uur/dag) |
| **Waste** | Afvalkalender: postcode+huisnummer (HVC) **of** een eigen ICS-kalender-URL |
| **Heating** | Temperatuur-offset, keteltype |
| **OT Bridge** | OpenTherm-brugmodus: Direct / Proxy / Wireless |
| **MQTT** | Broker host/poort/gebruiker + topics (banners) |
| **Presets** | Setpoints per programma (Comfort/Home/Sleep/Away) |
| **About** | Versie, status, diagnostiek, handmatige update-check |
| **Clean** | 30 sec schermvergrendeling om te poetsen |
| **Integrations** | Aan/uit per integratie (P1 elektra/water, ventilatie, HA, Z-Wave) |
| **UI mode** | freetoon vs stock qt-gui + boot-picker aan/uit |
| **Marketplace** | Integraties uit de catalogus installeren |
| **Tiles** | Marktplaats-integraties aan een tegel koppelen (incl. de 4 Pagina-2-slots) |
| **Z-Wave** | Ingebouwde Z-Wave-controller: apparaten in/uitsluiten, hernoemen |
| **WiFi** | Netwerken scannen/verbinden/loskoppelen + status |
| **Adapters** | Meteradapter (Z-Wave) + keteladapter (bedraad): status & test |
| **Domoticz** | Lampen + zonwering via de Domoticz JSON-API (HA-alternatief) |
| **Client mode** | Deze Toon als "slave" van een master-Toon (zie verderop) |
| **Auto-rotate** | Eén tegel laat zijn inhoud rouleren langs gekozen integraties + interval |
| **Newsreader** | RSS-ticker aan/uit + feed-URL (default NOS) |
| **Restart UI** | De interface herstarten (leest instellingen opnieuw) |

---

## Integraties (wat er aan databronnen werkt)

- **Energie** — via de ingebouwde **meteradapter** (officiële Z-Wave-meterlezing)
  *of* via een **HomeWizard P1** in het LAN. Instelbaar per Toon.
- **Water** — HomeWizard watermeter (HWE-WTR).
- **Ventilatie** — **Itho** (NRG-Itho-Wifi): stand tonen + bedienen
  (low/medium/high/auto/timer).
- **Home Assistant** — gordijnen/covers + lampen (via de lokale HA-API). De
  HA→Toon-koppeling levert ook een bestuurbare `climate.toon_thermostat` op aan
  HA-zijde.
- **Domoticz** — lampen (aan/uit/dim) en zonwering (open/stop/dicht) via de
  JSON-API, als alternatief voor Home Assistant.
- **Weer** — Buienradar (uur- en dagverwachting), stad→id automatisch opgezocht.
- **Afval** — HVC-inzamelkalender (postcode) of een generieke ICS-kalender.
- **Pakketten** — lokaal: vul tracking-code + vervoerder in en de juiste
  track-&-trace-URL wordt automatisch gebouwd; statuswijzigingen verschijnen als
  Toon-banner (geen HA nodig). Optioneel kan de bestaande HA-e-mailparser via
  MQTT pakketten automatisch aanvullen.
- **Nieuws** — RSS/Atom-ticker (zie boven).
- **Z-Wave** — de ingebouwde controller: apparaten beheren via de HCB-API.

---

## Marktplaats & eigen tegels

freetoon heeft een kleine **marktplaats**: integraties zijn losse pakketjes met
een `manifest.json` (naam, kleur, icoon, BoxTalk-service, welk veld de tegel-waarde
is). Je installeert ze via **Instellingen → Marketplace**, en koppelt ze daarna
aan een tegel via **Instellingen → Tiles** (of door op een tegel te
*long-pressen*, of op een lege **Pagina-2**-slot te tikken → "Open Marketplace").

De vier **Pagina-2-slots** (de veegpagina) zijn vrij toewijsbaar: elke slot toont
de waarde/titel/subtitel van de gekoppelde integratie. Met **Auto-rotate** kan één
slot zijn inhoud automatisch laten rouleren langs een door jou gekozen set
integraties, met instelbaar interval.

---

## Client mode (master/slave) + tablet

Een tweede Toon kun je als **client/slave** instellen
(**Instellingen → Client mode**, vul het IP van de master-Toon in). De slave start
dan **geen** eigen integraties: hij streamt de live-status van de master via diens
PWA-API en stuurt setpoint/programma/gordijnen terug naar de master. Hij praat
**alleen** met de master.

Hetzelfde idee werkt op een **Android-tablet** (of elke telefoon/PC): die heeft
niets nieuws nodig — open gewoon `http://<master-toon>:10081/` in de browser en
"voeg toe aan beginscherm". Het is een volwaardige PWA met dezelfde live-status en
bediening.

---

## Telefoonbediening (PWA) & instellingen in de browser

Elke Toon draait een kleine webserver op poort **10081**:

- `http://<toon-ip>:10081/` — de telefoon-/tablet-UI: temperatuur, setpoint,
  programma's, ketel, lucht, gordijnen en pakketten, live via Server-Sent-Events.
- `http://<toon-ip>:10081/settings` — alle instellingen in de browser (handig om
  zonder touch in te vullen).

---

## Op afstand kijken/bedienen (VNC)

Met **VNC** (poort 5900, in te schakelen onder Display options) kun je het scherm
1-op-1 op afstand zien én bedienen — handig voor support of configuratie zonder
voor het apparaat te staan.

---

## Wat werkt — en de eerlijke beperkingen

- ✅ Thermostaat, ketelinfo, lucht, meteradapter, ventilatie, weer, afval,
  gordijnen, lampen (HA/Domoticz), Z-Wave, WiFi, pakketten, nieuws, stats/grafieken,
  PWA, VNC, client mode, marktplaats — allemaal werkend.
- ⚠️ **Pakketstatus automatisch scrapen lukt niet** voor o.a. DHL: die zit achter
  Akamai Bot Manager, dus headless ophalen wordt geblokkeerd. Daarom is de
  pakkettenmodule "link-only" (officiële track-URL + Toon-banner). Wie automatische
  status wil, kan een gratis DHL API-key gebruiken — dat kan later ingebouwd worden.
- ⚠️ **Zone-/multi-room-verwarming** ondersteunt de Toon officieel niet: het is
  van origine een single-zone-thermostaat (één ruimte, één ketel). Zonering zou
  externe Z-Wave-radiatorknoppen + eigen logica vergen.

---

## Meedenken / wensen

Reacties, bugs en wensen zijn welkom. Populaire ideeën tot nu toe: extra
"default" integraties (nieuwslezer ✅, agenda, Sonos), meer marktplaats-tegels, en
fijnmazigere tegel-rotatie. Laat het hieronder weten.
