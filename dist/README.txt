TERRA NOVA: FREEFALL @VERSION@
==========================

Terra Nova: Freefall runs Terra Nova: Strike Force Centauri (Looking Glass
Technologies, 1996) natively on modern Windows, without DOSBox. It is built
from a full decompilation of the original game, so it plays exactly like the
DOS version, with optional improvements: Classic keeps them all off, Freefall
turns them all on (see THE OPTIONS SCREEN).

Please report anything that looks wrong (see "Reporting a problem" below).

Build: @VERSION@, commit @COMMIT@


WHAT YOU NEED
-------------
- Your own copy of Terra Nova: Strike Force Centauri, English version 1.09:
  the Steam or GOG release. Freefall does not contain the game: it reads
  the original files from your game folder.
- Windows 10 or 11 (tested). Windows 7 and 8 should work but are untested.


DOWNLOADS
---------
- Freefall (this zip, with the Freefall Mod Pack, the Freefall Music pack
  and the GeneralUser GS SoundFont inside):
    https://github.com/Cyber1on/terranova-freefall/releases
- AWE32 ROM (optional), for the game's own Sound Blaster AWE32 music: the
  file awe32.raw from the 86Box emulator's ROM collection,
    https://github.com/86Box/roms/tree/master/sound/creative
  Click awe32.raw there, then the "Download raw file" button (the arrow
  pointing down, on the right above the file), and put the file into the
  Music folder next to tnfree.exe. With Music: SoundFont (AWE32) on the
  options screen's Music & Movies page, all 58 sounds of the game's AWE32
  bank play, as on the card (see SOUNDFONT MUSIC).


INSTALLING
----------
1. Unzip this folder anywhere you can write to, for example
   C:\Games\Freefall. Not inside "Program Files", and not inside the game
   folder (both work less well).
2. Double-click tnfree.exe.
3. Freefall finds your Steam or GOG copy of the game by itself. If it
   finds none, a window asks for your game folder the first time. Choose
   the game's installation folder (it holds the TNOVA folder on Steam,
   the TN folder on GOG), usually:
     Steam: ...\steamapps\common\Terra Nova Strike Force Centauri
     GOG:   C:\GOG Games\Terra Nova
   The folder is remembered.
4. After the intro movies, at the game's menu, Freefall asks how you want
   to play: Freefall (recommended: the best way to play, every
   improvement on), Vanilla+ (modern controls, the classic 3D at 2x,
   60 fps) or Classic (the original game).
   You can change it any time on the options screen.

The first start is full screen (a window if full screen fails). Alt+Enter
switches between a window and full screen (the options screen's Fullscreen
line does too, and Freefall remembers it for the next start).

The Freefall Mod Pack comes with Freefall, in the Mods folder as
Freefall.zip: the cockpit and the goggles painted for 16:9 for every suit,
your suit's arms holding its weapons, new explosions and deaths, trees,
bushes and rocks. The Freefall preset uses it; Vanilla+ and Classic show
the game's own art. You can also choose it on the options screen's Mods
page (Mod set). Leave it in Mods as it is, without unzipping it.

The maps and movies are on the game's CD, which Steam and GOG keep in a
folder next to the game's (_ISO on Steam, CD on GOG). Freefall finds it
there and, from a CD image, copies what it needs once into its own CD
folder.

Freefall writes only next to tnfree.exe, never into your game folder or into
AppData:
  Saves\       the game's config and saved games
  CD\          the copy of the CD files
  Music\       the Freefall Music pack and the GeneralUser GS SoundFont;
               other music packs and SoundFonts (optional)
  Mods\        the Freefall Mod Pack (Freefall.zip); other mods, each one
               zip file (optional, see Mods\README.txt)
  tnfree.ini   Freefall's settings
  tnfree.log   messages (attach it when you report a problem)

Terra Nova Plus: if its HD mode has changed __FF.EXE, Freefall uses the
original copy that Terra Nova Plus keeps (__FF.EXE.tnplus-original). Your
Terra Nova Plus setup keeps working.


WINDOWS WARNINGS
----------------
Freefall is not digitally signed yet, so your browser and Windows
may warn about it:
- Chrome / Edge download: choose "Keep" (Chrome: the arrow next to the
  download, then "Keep dangerous file").
- "Windows protected your PC" (SmartScreen): click "More info", then
  "Run anyway".
Signed builds are planned for a later 1.x version.


THE OPTIONS SCREEN (O)
----------------------
Press O in the menus or in a mission, click "Freefall Options" at the
bottom left of the game's main menu, or use the FREEFALL OPTIONS button
on the game's own options screen (Alt+O; the button is in the middle above
its Cancel / Accept buttons). In a mission Esc opens it too when
the game has no use for Esc just then: Esc still closes the squad's orders
menu, answers No in a yes / no box and closes the help screen first, and
Ctrl+Esc still ends the mission. Esc, O or CLOSE closes it. H is the
help screen (below). Changes apply at once and are saved in tnfree.ini.
O is a letter while you type a saved game's name, and on the game's own
options screen it is that screen's Accept, as in the original. Alt+O opens
the game's own options screen (O did before), in the menus, the base and
missions, and closes it.

Clicks sound like the game's own options screen: its click on a button,
the Stereo sound buttons play the game's "left, right" test, and the FX
and Speech volume sliders play a sound at the new level, as on the game's
screen.

The screen is drawn in the style of the game's own options screen, with
its pointer. On the left: Fullscreen, Preset, the five settings pages, Mods
and Show FPS; the page's settings fill the middle and right columns, each a
row of buttons (the chosen one lit), a slider or a list with arrows. Click
a button (the mouse only: no keys but Esc, which closes it); the mouse
wheel changes the setting under the pointer. The line at the bottom says what the setting under the pointer
does. Under it: KEY BINDINGS, CLOSE, the FREEFALL box (opens the Freefall
website) and GAME OPTIONS (the game's own options screen, in a mission, or
back to it when you came from its FREEFALL OPTIONS button).
The Display, Distance & Detail and Cockpit & Maps pages show a picture of
the game with the setting under the pointer, or the one you chose.

The first time the game's menu shows, Freefall asks how you want to play:
Freefall (recommended), Vanilla+ or Classic, side by side with a picture of
each and what it changes (Left/Right and Enter, or click anywhere on a
card; it plays the sound of the outfitting's Review Video Briefing). Until you
choose, everything is off (Classic). Change it any time with Preset.

The left column:
  Preset              Freefall (recommended), Vanilla+ or Classic, see the
                      list below. Custom lights up once you change any of
                      the options a preset sets; choose a preset again to
                      go back to it. While something the preset turned on
                      is still starting (Smooth videos, Anti-aliasing) the
                      help line says "Starting..." for a moment.
  Fullscreen          Off (a window you can resize) or On (the game fills
                      the screen; On at the first start). Alt+Enter
                      switches it too; the next start keeps it. The
                      presets leave it as it is.
  Gameplay            Controls style, auto-aim, mouse, mission select,
                      sound volumes and stereo
  Display             Frame rate, 3D resolution, widescreen, scaling,
                      anti-aliasing, filters
  Distance & Detail   How far the land, objects and enemies show, fog,
                      terrain smoothing, sky, water, trees, infrared
  Cockpit & Maps      Target label, target camera, the maps
  Music & Movies      Music, music pack, smooth videos, movie size
  Mods                Mods (zip files) from the Mods folder
  Show FPS            Game and display frames a second and the 3D drawing
                      time, in the window's top right corner (also a key
                      you can set in Key bindings).

What the presets set (Fullscreen, Mouse sensitivity, Invert mouse, keys,
Filters, Show FPS and Music stay as you set them):

                       Classic      Vanilla+         Freefall
  Frame rate           25 fps       60 fps           60 fps
  Controls style       Classic      Advanced         Advanced
  Auto-aim             20 px        5 px             5 px
  Auto-aim highlight   Off          Off              On
  Auto target camera   Off          On               On
  Target label         Classic      Around the box   Around the box
  Head view cone       Off          On               On
  Show enemies         Off          Off              On
    without MFD
  3D resolution        Classic      2x               4x
  Widescreen           Off          Off              On
  Scaling              Fit          Fit              Sharp
  Anti-aliasing        Off          Off              FXAA
  Terrain distance     Classic      High             High
  Object distance      Classic      Far              Farthest
  Enemy distance       Classic      Classic          Far
  Fog                  Off          On               On
  Terrain smoothing    Off          Off              On
  True colour          Off          Off              On
  Water reflection     Low          Medium           Medium
  Water ripples        Off          On               On
  Water effects        Off          On               On
  Map smoothing        Off          Off              Off
  Tree size            Classic      Classic          Realistic
  Tree explosion       Classic      Classic          Realistic
  Infrared             Classic      Sci-fi           Sci-fi
  Smooth videos        Off          30 fps           30 fps
  Movie size           Classic      Classic          Classic
  Mod set              Off          Off              Freefall (the
                                                     Freefall Mod Pack,
                                                     Mods\Freefall.zip)
  Dynamic helmet       Off          Off              On
  Music pack           Off          Off              Freefall-MP3 (the
                                                     Freefall Music pack,
                                                     see FREEFALL MUSIC;
                                                     else Off)

An option that needs another one shows grey until that one is on.

  Gameplay
    Controls style      Classic (the game's own controls) or Advanced
                        (recommended: mouselook, see below).
    Auto-aim            How near a locked target (T, Y or a right click
                        on it) the pointer must be for the shot to go to
                        the target itself, ahead of it for beams and
                        lobbed onto it for grenades and missiles:
                        20 px (classic) or 5 px from the target's box
                        (game pixels, twice that up and down with the
                        goggles). 20 px needs the Classic controls style:
                        with Advanced it shows grey and works as 5 px. A
                        click in a target camera's screen always uses the
                        lock. Off: shots always go where you point. The
                        target's top and bottom ticks double while a shot
                        would go to it.
    Auto-aim highlight  Off or On: while a shot would go to the locked
                        target, the crosshair's bright green middle turns
                        red. Grey with Auto-aim Off.
    Mouse sensitivity / Invert mouse
    Mission select      Which missions the Mission Selector lets you
                        start: Reached (the default: the missions your
                        campaigns have reached) or All (cheat: every
                        campaign mission). Not part of the presets.
    Sound               FX, Music and Speech volume and Stereo sound
                        (Normal or Reversed): the game's own sound options,
                        the same as on its options screen (Alt+O).
    KEY BINDINGS        (the button at the bottom) Choose your own keys and
                        mouse buttons for missions, for each controls
                        style; DEFAULT KEYS brings back the
                        style's own.

  Display
    Frame rate          25 fps (classic) or 60 fps instead of the game's
                        25. The original game's shots stop hitting above
                        about 30 frames a second; Freefall fixes that at 60
                        fps. The cockpit's 3D screens are then drawn every
                        frame too (the game draws them in turns, each at a
                        third of the frame rate).
    3D resolution       The 3D view and the maps drawn at 2, 4 or 6 times
                        the game's pixels; the target's label sharp, at 3/4
                        of the game's size.
    Widescreen          In missions the screen stretched to the window's
                        width, the 3D and the maps wide at the right
                        proportions. The main menu fills a 16:9 window:
                        the stars and the planet go on beside the game's
                        picture (art by Cyberion).
    Scaling             Fit (classic) or Sharp (even pixels at the same
                        size).
    Anti-aliasing       Off or FXAA: the stairs along edges in the sharp 3D
                        views smoothed on the graphics card (needs 3D
                        resolution 2x, 4x or 6x and Direct3D 12 or Vulkan;
                        the HUD, text, maps and the target's label stay
                        crisp).
    GPU rendering       Off or On (experimental): the graphics card draws
                        the main 3D view's ground, near and far, in full
                        colour and perspective-correct (its textures no
                        longer swim as you move), the fog for every pixel,
                        and the rain and snow as particles in the world
                        around you.  Always in full colour (True colour
                        On).  The sky, objects and water are still the
                        game's.  Needs Direct3D 12.  Not part of the
                        presets.
    Filters             (the right column) Screen filters, only how the
                        picture looks:
                          Brightness, Gamma, Contrast, Colour
                          Scanlines: dark lines between the game's lines
                          Effect: Smooth edges or CRT / CRT curved (a CRT
                          monitor look). Effects use the graphics card
                          through Direct3D 12 or Vulkan; "not available"
                          means the PC has neither.

  Distance & Detail
    Terrain distance    Classic (the game's), Medium or High: smooth land
                        out to 12, 32 or 48 squares.
    Object distance     Classic (the game's), Far or Farthest: units,
                        vehicles and buildings drawn twice or 4 times as
                        far as the game draws them, trees out to 48 or 96
                        squares; enemies that can shoot follow Enemy
                        distance.
    Enemy distance      Classic (the game's) or Far: the enemy's
                        soldiers, vehicles, aircraft and shots twice as far
                        (gun turrets always the game's).
    Fog                 Off or On: the far land fades into the sky's colour
                        and is not drawn past 240 squares; nearer and
                        thicker in rain and snow; in the infrared view the
                        far land fades into the infrared's own haze colour.

    Terrain smoothing   The land beyond the polygons textured like the near
                        ground (no noise or stairs), and the near ground's
                        light smooth instead of in bands.
    True colour         Off (the game's 256 colours), On (the sky, clouds
                        and fog of the 3D view in full colour: soft clouds,
                        a smooth sky and fog, no dots) or On + sun glow
                        (experimental: and a warm glow around the sun on
                        the sky and the fog). Costs time: about 20 ms a
                        frame at 6x widescreen on a 4-core PC, less at
                        lower 3D resolutions. Not in the infrared view.
    Water reflection    Low (the game's default), Medium (the sky in all
                        the water, near and far, at any angle) or High
                        (experimental) (Medium, and land, buildings and
                        units mirrored where they stand).
    Water ripples       Off or On (moving waves on the water and rings
                        around soldiers in it); needs Water reflection:
                        Medium or High.
    Water effects       Off or On: splashes where shots and bodies hit the
                        water, wakes behind vehicles, lighter, light blue
                        water in the shallows of big lakes and seas (needs
                        Water reflection: Medium or High).
    Tree size           Classic (the game's) or Realistic: every tree,
                        bush and rock its own size, 0.6 to 2 times, by its
                        place on the map; the picture only (shots and bumps
                        as the game's).
    Tree explosion      Classic (the game's fireball) or Realistic (a
                        dying tree or bush bursts into burning pieces; what
                        is left of it stays standing until the mission
                        ends, the picture only: you can walk and shoot
                        through it); a tree that breaks first (13 kinds of
                        tree and bush, which in the game take two hits)
                        bursts whole too and is gone from the game at once,
                        so nothing unseen is left to bump into or shoot
                        again (the one change to play: Classic keeps the
                        game's broken tree); the blast, leftovers, chips,
                        smoke and dust are the effects pack's, and so are
                        the blast marks on the ground when it has them
                        (see Mods\README.txt).
    Infrared            Classic (the game's) or Sci-fi: the infrared view
                        (I key) as a thermal camera, cold ground blue up to
                        white-hot units and fires; trees, plants and the
                        ground stay cool.

  Cockpit & Maps
    Target label        Classic or Around the box: the target's name to
                        the right of it, its distance above its box and
                        "Locked" or "Out of range" under its health bar.
    Auto target camera  The left screen shows the target camera when you
                        pick a target, and again after squad talk or orders
                        (F1-F4) close while the target is still picked.
    Head view cone      The squad map and the full map (M) shade where your
                        head looks; the white tick still shows where the
                        suit faces. As wide as the view (zoom, widescreen).
                        In the goggles with the side screens hidden (Tab) the
                        cone and your mark stand still where the middle
                        screen's map is, in its middle.
    Show enemies without MFD
                        Off or On: in the goggles with the side screens
                        hidden (Tab), the enemies the middle screen's map
                        shows are drawn around your mark at the map's scale
                        (no map, no squad). On in the Freefall preset.
                        Grey while Radar without MFD is on.
    Map smoothing       Off (the game's) or On (smooth coasts, roads and
                        hills, the game's flat colours); needs 3D
                        resolution 2x, 4x or 6x.
    Radar without MFD (Experimental)
                        Off or On: in the goggles with the side screens
                        hidden (Tab), the middle screen's whole map as a
                        round radar where that screen is, north up: its
                        land faded into the view, its marks (enemies, squad,
                        nav points, your mark in the middle), the view cone
                        out to the green rim, and arrows outside the rim
                        for enemies and the nav point beyond it. It uses
                        the map's zoom. Needs 3D resolution 2x, 4x or 6x;
                        in no preset. Experimental: it may change.

  Music & Movies
    Music               General MIDI (Windows' MIDI synthesizer),
                        SoundFont (AWE32) (the Sound Blaster AWE32 sound, see
                        SOUNDFONT MUSIC), OPL3 FM (the Sound Blaster 16 /
                        Pro 2 sound), OPL2 FM (the AdLib / first Sound
                        Blaster sound) or Off; heard at once, the piece
                        carrying on (as --music). At first it shows what the
                        game's own sound setup chose.
    SoundFont for AWE32 The SoundFont the SoundFont music plays: each .sf2
                        or .dls file in the Music folder, and Windows' own
                        gm.dls. The help line says how much of the game's
                        AWE32 bank plays over it. Grey unless Music is
                        SoundFont (AWE32).
    Music pack          OGG or MP3 music in place of the game's music, a
                        folder or a zip in the Music folder (see
                        Music\README.txt). Freefall-MP3 is the Freefall
                        Music pack (see FREEFALL MUSIC); the Freefall preset
                        chooses it, Vanilla+ and Classic choose Off (the
                        game's own music). Mod sets never change it.
    Smooth videos       Off or 30 fps: the movies at 30 pictures a second
                        instead of 15; the graphics card makes the picture
                        between each two (RIFE). Needs Vulkan graphics
                        drivers ("not available" without them).
    Movie size          Classic (the movies their own size, as in the
                        game) or Fill screen: each movie as big as the
                        window allows (the briefings twice their size).

  Mods
    Mod set             Off, or a mod: a zip file in the Mods folder with
                        cockpits, arms, effects and objects (any of them),
                        all chosen at once (see Mods\README.txt). Shows
                        Custom when the packs below are from more than one
                        mod.
                        The four lines below show each mod that has that
                        part, by the mod's name, so parts of different
                        mods can be mixed.
    Cockpit pack        Off (the stretched cockpit) or a pack: with
                        Widescreen, the cockpit and the goggles painted for
                        16:9, the game's screen 1:1 in the middle, the 3D
                        through the pack's wider window; narrower windows
                        get bars above and below (see Mods\README.txt).
    Dynamic helmet      Off or On (grey without Widescreen and a cockpit
                        pack; it needs a pack whose pictures come in
                        parts, as the Freefall mod's standard suit does):
                        the helmet is worn, so it moves a little against
                        your head: it lags behind a turn or a look up or
                        down and springs back, bobs with your steps, dips
                        when you land, kicks when you fire and shakes when
                        you are hit (the screens break into static for a
                        moment). Its parts
                        (chin, bezels, brow, walls, plates, upper corners)
                        move by their distance from your eye, so the frame
                        looks deep. The 3D view, the crosshair and the
                        targets' boxes stay still, so you aim where you
                        see; the HUD's displays in the frame (the weapon
                        screen, the numbers by the health bar) move with
                        their part. Only the picture moves: the game, the
                        HUD's buttons and the mouse are where they were.
    Arms pack           Off or a pack: in the goggles view your suit's arm
                        holding the selected weapon (the left one
                        mirrored), aimed at the pointer, swaying with
                        mouselook, a flash and a kick when you fire (see
                        Mods\README.txt).
    Effects pack        Off (the game's) or a pack: explosions and other
                        effect animations drawn with the pack's pictures,
                        their half see-through pixels (soft smoke) blended
                        with what is behind; trees, rocks, soldiers and
                        vehicles can have deaths of their own, and the
                        ground its blast marks (see Mods\README.txt).
    Objects pack        Off (the game's) or a pack: trees, bushes and rocks
                        drawn with the pack's pictures, coloured for each
                        planet as the game's own (see Mods\README.txt).

The game's own options (Alt+O) start with two Freefall defaults: Resolution
320 X 400 (missions open in the high detail screen with the finer HUD) and
Ground detail Excellent. Change them there as in the original. Loading a
saved game keeps your Resolution and detail settings (the original put back
the ones saved with the game).

Two fixes to the original game are always on:
- Units move at the speed the game intends, smoothly, at every frame rate. The
  original's physics clock made them about a third faster at 25 to 32 frames
  a second (G17 in Docs\BUGS.txt).
- Every kill shows its explosion and body. In big fights the original ran out
  of room for effects, and some enemies vanished with only their sound (G19).


ADVANCED CONTROLS (mouselook)
-----------------------------
The aiming reticle stays at the centre of the view; the mouse moves your
head (the game's Q/E and R/V) without changing where you walk. Left and
right click fire and target at the reticle, as in the original.

  Mouse                look around
  F                    Align body to headview: turn to face where you look
  X                    Gimbal mode on/off: keep turning to face where you
                       look, as if F were held ("GIMBAL MODE: ON" shows)
  Middle button        free the pointer for the HUD / capture it again
  Left Shift (hold)    free the pointer while held
  Mouse wheel          zoom in / out
  W / S                forward / back
  Z                    forward at half speed
  A / D                turn left / right
  Q / E                strafe left / right

Everything else stays on the game's own keys. Each controls style keeps its
own key bindings; on the Key bindings page, DEFAULT KEYS brings back the defaults.
The pointer is also free while the map fills the view (M); there the wheel
zooms the map, and dragging with the right button moves it, as in the
original game.


THE HELP SCREEN (H)
-------------------
For the first seconds of a mission a white line on the view names the keys
to know first (H, O and, with the Advanced controls, F). It stops coming
once you have opened the help screen. The Classic preset never shows it.

Press H (or ?) in a mission. With the Classic preset it is the game's own
help screen. With any other settings it is Freefall's: your own cockpit or
goggles as they were when you pressed H, each part named with its key, and
the controls of your controls style (Classic or Advanced) in five lists. The
keys shown are the ones you set on the Key bindings page. Press H again for the
game's own help screen; any other key or a click goes back to the mission.


FREEFALL MUSIC
--------------
Freefall comes with the Freefall Music pack (Music\Freefall-MP3.zip): the
game's music recorded as MP3 by Rich "Weeds" Nagel (http://www.richnagel.net),
shared with his permission. The Freefall preset plays it; Vanilla+ and
Classic play the game's own music, as the Music line sets it. Choose it or
Off any time on the options screen's Music & Movies page, Music pack. Keep
the zip as it is: Freefall reads it without unzipping.


SOUNDFONT MUSIC
---------------
Music: SoundFont (AWE32) plays the music as a Sound Blaster AWE32 did. The
AWE32 had General MIDI sounds of its own, and the game put its own AWE32
bank over them (SOUND\FF.SBK, "TerraNova GM" by Eric Brosius). Freefall
plays the music with FluidSynth: the General MIDI sounds come from a
SoundFont, the game's bank goes over it.
- The SoundFont: Freefall comes with GeneralUser GS 2.0.3 by S. Christian
  Collins in the Music folder (GeneralUser-GS.sf2, its licence beside it).
  Any other General MIDI SoundFont (.sf2) or DLS file (.dls) put in the
  Music folder can play instead, chosen on the SoundFont for AWE32 line (the
  first by name until you choose one), and so can Windows' own gm.dls (the
  same sounds as General MIDI, without the Windows MIDI synthesizer's
  delay). Other free SoundFonts that sound good: FluidR3 GM, Arachno.
- The game's bank: most of its sounds are made from the AWE32's ROM samples,
  which no game file has. Without them 2 of its 58 sounds play (dulcimer and
  overdrive guitar) and the SoundFont plays the others. With a dump of the
  AWE32's 1 MB ROM in the Music folder (awe32.raw, the file emulators use;
  where to get it: see DOWNLOADS) all 58 play, as on the card. Nothing to
  choose: Freefall finds the file by itself, and the SoundFont for AWE32
  line's help then says "AWE32.RAW installed, all 58 sounds in use"
  (without it: "No AWE32.RAW: 2 of 58 sounds in use").
- No SoundFont at all (GeneralUser GS taken out of Music, not on Windows):
  the music plays FM (OPL3) instead.


BENCHMARK
---------
Double-click tnfree-benchmark.bat (or start tnfree.exe --bench) to see how
fast your PC draws the game with your settings. It plays a fixed course in
mission 3 by itself, about a minute: open ground, the lake in the goggles,
the lake in the cockpit and a forest, walking forward. It saves no game and
leaves your settings as they are. Esc stops it.

At the end a window shows a frame's time and the frames a second for each
scene (what your PC could draw; the game itself keeps to 25, or 60 with
Frame rate: 60 fps). Each run is added to tnfree-bench.txt next to
tnfree.exe, with your settings, so you can change one option, run it again
and compare. The game's own options (Alt+O) count too: keep them the same
between runs you compare.


COMMAND LINE AND tnfree.ini
---------------------------
  --game DIR       the TNOVA folder (instead of the search and the folder
                   window)
  --cd DIR         the CD contents or CD image, if not found automatically
  --user DIR       keep saves, CD copy and tnfree.ini in DIR instead
  --music M        gm (Windows General MIDI), soundfont (AWE32), opl3
                   (Sound Blaster 16 FM), opl2 (AdLib FM) or off
  --fps N          at most N frames a second (default 25; 0 = no limit)
  --mouselook      Advanced controls
  --preset P       choose a preset at start: classic, vanilla+ or freefall
                   (saved, as choosing it on the options screen)
  --mission N      New Campaign starts at mission N (see STARTING AT A
                   MISSION)
  --bench          the benchmark (see BENCHMARK)
In tnfree.ini: smooth_fps=true, controls=advanced, mouse_sensitivity=100,
mouse_invert=on, auto_target_camera=on, music_pack=<folder>,
soundfont=<file>, screen_effects=<folder>, screen_objpack=<folder>,
screen_trees=realistic.


MISSION SELECTOR
----------------
Click "Mission Selector" in the middle of the game's main menu (above the
credit line) to start any campaign mission you have reached. The screen is
in the style of the game's Random Scenario Builder:
- Left: the campaign's 39 missions in order, 17 a page (the arrows under
  the list, or Page Up / Page Down, turn the page). A film strip and "FMV"
  mark a mission with a live-action story scene. Missions you have not
  reached yet show "Not reached yet".
- Right: the mission's name, its place in the campaign and the suit you
  wear, a map of the mission area in full colour (the arrows switch to the
  whole map) with the drop point, the map's labels and the forces (green
  allied, red hostile, grey neutral), the game's full briefing text (the
  arrows, the bar or the mouse wheel scroll it), "Video Briefing" and
  "Story FMV" to watch the mission's movies in the map's box (under it
  Stop stops the movie, Fullscreen shows it as big as "Movie size" on
  the Sound page says; "Smooth videos" works here too; a key or a click
  there goes back to the box; Esc
  stops it; the music stops while one plays), and your record: completed
  or not, medals and best score.
- Bottom: Cancel (or Esc, or a right click) goes back, Freefall Options
  opens the options screen over it, Play Mission (or Enter, or a double
  click on the mission) starts it.
Play Mission starts a new campaign at that mission, as New Game does, and
takes you straight to the mission's outfitting: the campaign's opening
movie, your quarters, the base and the briefing video are passed for you,
silently, under a black screen with the mission's name
(watch the movies in the selector; the outfitting has Review Video
Briefing). A "New Technology" screen the mission brings still shows. Then
the mission, and the campaign goes on from there as usual. (The two
training missions start as New Game does, with their instructions.) The missions before it count as not played and your squad
is the one a new campaign starts with; saved games are not changed.
Up / Down choose a mission, Left / Right switch the map.

Which missions count as reached: the furthest place in the campaign your
games have got to, kept in Saves\missions.ini (with your record for each
mission). The first time, Freefall works it out from your saved games.
The first mission after the training (Operation Cannonball) and the two
training missions are always there. "Mission select: All" on the Gameplay
page lists every mission (for testing).

STARTING AT A MISSION
---------------------
Start tnfree.exe with --mission N (for example in a shortcut: right-click
tnfree.exe, Create shortcut, then in the shortcut's Properties add
--mission 39 after the Target's tnfree.exe), choose New Game in the menu,
and the campaign starts at mission N: its briefing and outfitting, then the
mission, and the campaign goes on from there as usual. The missions before
it count as not played (no statistics or medals) and your squad is the one
a new campaign starts with. Without --mission New Game starts at the
beginning; saved games are not changed. The game's own setting, which the
original never let you set.

The campaign in order, with the game's mission numbers and the suit the
mission gives you (Operation Ivanhoe brings the Heavy suit, Operation Cold
War is the only mission in the pirate suit):

  No.  Mission                    Suit       No.  Mission                    Suit
  46   XR-5 Qualification         Standard   16   Operation Seraphim         Heavy
  47   Combat Certification       Standard   36   Operation Cropduster       Heavy
  37   Operation Cannonball       Standard   14   Operation Mercury          Standard
  38   Operation Panama           Standard   20   Operation Black Widow      Standard
   1   Operation Eagle Eye        Scout      39   Operation Cold War         Pirate
   2   Operation Sherwood         Standard   21   Operation Aegis            Heavy
   3   Operation Seagull          Standard   35   Operation Buzzard          Heavy
   4   Operation Eagle's Roost    Standard   22   Operation Sparkplug        Heavy
   5   Operation Valkyrie         Standard   23   Operation Cadmium          Heavy
   6   Operation Eagle's Nest     Standard   19   Operation Bloodline        Heavy
   7   Operation Mariner          Standard   24   Operation Fusebox          Heavy
   8   Operation Periscope        Scout      27   Operation Calais           Heavy
   9   Operation Ivanhoe          Heavy      25   Operation Pigpen           Heavy
  32   Operation Meteor           Standard   26   Operation Hyperbole        Heavy
  33   Operation Meteorite        Standard   30   Operation Icepick          Heavy
  10   Operation Oilslick         Heavy      31   Operation Absolute Zero    Heavy
  11   Operation Market Square    Standard
  12   Operation Eggshell         Standard
  13   Operation Conductor        Standard
  34   Operation Meteor Shower    Heavy
  29   Operation Goblin           Standard
  15   Operation Undertow         Standard
  17   Operation Ivy              Standard


KNOWN ISSUES
------------
- Not signed yet: browser and SmartScreen warnings (see above).
- General MIDI music (--music gm) uses Windows' built-in synthesizer, which
  sounds thin; OPL3 FM (--music opl3) is the classic Sound Blaster sound.
  SoundFont music with a good SoundFont sounds much fuller.
- The head-mounted displays of 1996 are not supported.
- Windows only for now. Linux and macOS come later.


REPORTING A PROBLEM
-------------------
Report it on GitHub (github.com/Cyber1on/terranova-freefall, the Issues
tab) or on the TTLG forums (ttlg.com/forums), with:
- tnfree.log (next to tnfree.exe; after a crash it names where it happened),
- which mission, and what you did just before,
- your options (or tnfree.ini),
- a screenshot if something looks wrong.
Docs\BUGS.txt lists every bug fixed so far, in the original game and in
the port.
Docs\RELEASE-NOTES.txt says what is new in each version.


LICENCE
-------
Terra Nova: Freefall is free software under the GNU General Public License,
version 3 (Docs\LICENSE.txt). Third-party parts: Docs\THIRD-PARTY.txt.
Terra Nova: Strike Force Centauri is (c) its rights holders; you need your
own copy of the game. Freefall is a fan project, not affiliated with
Looking Glass Technologies, Nightdive Studios or Atari.
