"""Export an Applio/RVC v2 voice model, and the ContentVec feature model, to ONNX for Sudz.

    python sudz_export.py voice  <model.pth> <out.onnx> [<logs/voice/extracted>]   # your voice (about 125 MB with the likeness boost)
    python sudz_export.py content <contentvec_dir> <out.onnx> # the shared feature model (about 360 MB, once)

Run it from inside an Applio checkout (it imports Applio's model code). MIT licence, like Applio and RVC.
"""
import json, os, sys
import torch

sys.path.insert(0, os.getcwd())


class SynthForSudz(torch.nn.Module):
    """RVC inference as one graph: features + pitch in, audio out. `rnd` is the prior noise.

    With a feature bank (your training recordings, clustered), each moment is first pulled towards the 8 closest
    matching moments of your own singing, as Applio's index does: `index_rate` sets how far (0 = off, 0.75 = Applio's
    usual), and `protect` keeps consonants and breaths (unpitched frames) closer to the original (0.5 = off)."""

    def __init__(self, net, bank=None):
        super().__init__()
        self.net = net
        self.has_bank = bank is not None
        if self.has_bank:
            self.register_buffer("bank", bank)
            self.register_buffer("bank_sq", (bank * bank).sum(1))

    def retrieve(self, phone, pitchf, index_rate, protect):
        p = phone[0]                                                     # (T, 768)
        d2 = (p * p).sum(1, keepdim=True) - 2.0 * (p @ self.bank.t()) + self.bank_sq.unsqueeze(0)
        d2 = torch.clamp(d2, min=1e-6)
        val, ix = torch.topk(d2, 8, dim=1, largest=False)
        w = torch.reciprocal(val) ** 2
        w = w / w.sum(1, keepdim=True)
        r = (self.bank[ix] * w.unsqueeze(-1)).sum(1)                     # (T, 768)
        f = r * index_rate + p * (1.0 - index_rate)
        pf = torch.where(pitchf[0] > 0, torch.ones_like(pitchf[0]), protect.expand_as(pitchf[0]))
        pf = torch.where(protect >= 0.5, torch.ones_like(pf), pf)
        f = f * pf.unsqueeze(-1) + p * (1.0 - pf.unsqueeze(-1))
        return f.unsqueeze(0)

    def forward(self, phone, phone_lengths, pitch, pitchf, sid, rnd, index_rate=None, protect=None):
        if self.has_bank:
            phone = self.retrieve(phone, pitchf, index_rate, protect)
        g = self.net.emb_g(sid).unsqueeze(-1)
        m_p, logs_p, x_mask = self.net.enc_p(phone, pitch, phone_lengths)
        z_p = (m_p + torch.exp(logs_p) * rnd * 0.66666) * x_mask
        z = self.net.flow(z_p, x_mask, g=g, reverse=True)
        return self.net.dec(z * x_mask, pitchf, g=g)


def feature_bank(features_dir, k=4096):
    """Your training recordings' content features, clustered to at most k points (about 12 MB)."""
    import glob
    import numpy as np

    files = sorted(glob.glob(os.path.join(features_dir, "*.npy")))
    if not files:
        return None
    x = np.concatenate([np.load(f) for f in files], axis=0).astype(np.float32)
    if x.shape[0] > k:
        from sklearn.cluster import MiniBatchKMeans

        x = MiniBatchKMeans(n_clusters=k, batch_size=256 * 8, n_init=3, random_state=0).fit(x).cluster_centers_.astype(np.float32)
    print("feature bank:", x.shape)
    return torch.from_numpy(x)


def _patch_for_export():
    # Applio's LayerNorm reads the channel count from the tensor, which the ONNX exporter can't trace; use the weights' size
    from rvc.lib.algorithm import normalization

    def forward(self, x):
        x = x.transpose(1, -1)
        x = torch.nn.functional.layer_norm(x, (self.gamma.shape[0],), self.gamma, self.beta, self.eps)
        return x.transpose(1, -1)

    normalization.LayerNorm.forward = forward


def export_voice(pth, out, features_dir=None):
    _patch_for_export()
    from rvc.lib.algorithm.synthesizers import Synthesizer

    cpt = torch.load(pth, map_location="cpu", weights_only=False)
    cfg = list(cpt["config"])
    cfg[-3] = cpt["weight"]["emb_g.weight"].shape[0]
    if not cpt.get("f0", 1) or cpt.get("version", "v2") != "v2":
        raise SystemExit("Sudz needs a v2 model trained with pitch guidance (the Applio default).")
    vocoder = cpt.get("vocoder", "HiFi-GAN")
    if vocoder != "HiFi-GAN":
        raise SystemExit("Sudz supports the HiFi-GAN vocoder (the Applio default); this model uses " + vocoder)
    net = Synthesizer(*cfg, use_f0=True, text_enc_hidden_dim=768, vocoder=vocoder, randomized=False)
    net.load_state_dict(cpt["weight"], strict=False)
    net = net.float().eval()
    net.remove_weight_norm()
    bank = feature_bank(features_dir) if features_dir else None
    m = SynthForSudz(net, bank).eval()
    T = 200
    args = (torch.randn(1, T, 768), torch.tensor([T]).long(), torch.randint(1, 255, (1, T)).long(),
            torch.rand(1, T) * 300 + 100, torch.tensor([0]).long(), torch.zeros(1, 192, T))
    names = ["phone", "phone_lengths", "pitch", "pitchf", "sid", "rnd"]
    if bank is not None:
        args = args + (torch.tensor([0.75]), torch.tensor([0.33]))
        names = names + ["index_rate", "protect"]
    with torch.no_grad():
        torch.onnx.export(m, args, out, opset_version=17, do_constant_folding=True, dynamo=False,
                          input_names=names, output_names=["audio"],
                          dynamic_axes={"phone": {1: "T"}, "pitch": {1: "T"}, "pitchf": {1: "T"}, "rnd": {2: "T"}, "audio": {2: "S"}})
    meta = {"sudz_voice": 1, "sr": int(cfg[-1]), "name": cpt.get("model_name", os.path.splitext(os.path.basename(pth))[0]),
            "epoch": cpt.get("epoch"), "speakers": int(cfg[-3]), "hop": int(cfg[-1]) // 100,
            "index": int(bank.shape[0]) if bank is not None else 0}
    with open(out + ".json", "w") as f:
        json.dump(meta, f, indent=1)
    print("voice exported:", out, meta)


class ContentForSudz(torch.nn.Module):
    def __init__(self, model):
        super().__init__()
        self.model = model

    def forward(self, audio16k):
        return self.model(audio16k)["last_hidden_state"]


def export_content(model_dir, out):
    from rvc.lib.utils import HubertModelWithFinalProj

    model = HubertModelWithFinalProj.from_pretrained(model_dir).float().eval()
    m = ContentForSudz(model).eval()
    with torch.no_grad():
        torch.onnx.export(m, (torch.randn(1, 16000),), out, opset_version=17, do_constant_folding=True, dynamo=False,
                          input_names=["audio"], output_names=["features"],
                          dynamic_axes={"audio": {1: "N"}, "features": {1: "T"}})
    print("content model exported:", out)


if __name__ == "__main__":
    kind, src, dst = sys.argv[1:4]
    feats = sys.argv[4] if len(sys.argv) > 4 else None   # voice: Applio's logs/<voice>/extracted folder, for the likeness boost
    export_voice(src, dst, feats) if kind == "voice" else export_content(src, dst)
