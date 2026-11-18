# Train your voice for Sudz

Sudz can re-sing any vocal (a guide, a demo, an AI vocal) in your own voice: **Pitch › Sing it in my voice**. First it needs a voice model trained on your singing. You do that once, for free, in Google Colab, and it takes about 1–2 hours.

## 1. Record your training vocals

- **15–30 minutes** of you singing on your own: no backing track, no reverb or effects. Several songs is better than one long one.
- Cover your **whole range**: low and high, soft and loud, held notes and quick phrases. The model can only reproduce what it has heard.
- Same set-up as your normal recordings (your AT2020 and UMC22 are fine). Avoid clipping and background noise.
- WAV or AIFF files. Takes you have already recorded for songs count, as long as they are dry.

## 2. Train it in Colab

1. Open the notebook: **[Sudz: train your voice](https://colab.research.google.com/github/davidsudwarts-hub/sudz/blob/main/voice/Sudz_Train_Your_Voice.ipynb)** and sign in with your Google account.
2. Choose **Runtime › Change runtime type › T4 GPU**.
3. Run cells **1** and **2** (the ▶ button on each). Cell 2 connects your Google Drive and makes a folder **Sudz Voice › my vocals**.
4. Put your vocal files in **My Drive › Sudz Voice › my vocals** (or tick *upload_now* in cell 4).
5. Run cells **3** to **7** in order. Cell 6 (training) is the long one. If Colab disconnects, run cells 1–4 again, then 6: it carries on from where it got to.
6. Optional: cell **8** lets you hear a short conversion in Colab before downloading.

When it finishes, **My Drive › Sudz Voice › export** contains:

| File | Size | What it is |
|---|---|---|
| `MyVoice.onnx` and `MyVoice.onnx.json` | about 110 MB | your voice (the name is whatever you chose in cell 4) |
| `sudz_content.onnx` | about 380 MB | the shared feature model, needed once |

## 3. Load it into Sudz

Download those files to your Mac. Then in Sudz (the app, the GarageBand plug-in, or the web version):

1. Open **Pitch › Sing it in my voice…**
2. **Add a voice model…** and choose your `.onnx` file. On the web version, select the `.onnx` and the `.onnx.json` together.
3. **Choose sudz_content.onnx…** (once; every voice uses it).

## 4. Use it

1. Open the vocal you want re-sung (a dry, single voice works best).
2. **Pitch › Sing it in my voice**: pick your voice and press **Match my range**. If the other singer sings higher or lower than you, this moves it by whole octaves, so the key stays the same. Use the slider for anything else.
3. **Convert this take to my voice.** The result becomes the take you are editing: tune and polish it as usual.

On the Mac it runs on the native AI engine, roughly in real time (a 3-minute song takes about 3 minutes). In the browser it is several times slower. In the GarageBand plug-in, the converted vocal replaces the transferred part and is saved with the song.

## Good to know

- It changes the **voice**, not the **performance**: phrasing, vibrato and style come from the vocal you convert.
- Quality depends mostly on the training vocals: more singing across more of your range gives a better voice.
- Only train on your own voice, or on singers who have given you permission, and only convert vocals you have the right to use.

Training uses [Applio](https://github.com/IAHispano/Applio) 3.7.1 (MIT), which builds on RVC (MIT) and ContentVec (MIT). `sudz_export.py` (MIT) converts the trained model for Sudz.
