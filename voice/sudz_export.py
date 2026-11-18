"""Export an Applio/RVC v2 voice model, and the ContentVec feature model, to ONNX for Sudz.

    python sudz_export.py voice  <model.pth> <out.onnx>      # your trained voice (about 110 MB)
    python sudz_export.py content <contentvec_dir> <out.onnx> # the shared feature model (about 360 MB, once)

Run it from inside an Applio checkout (it imports Applio's model code). MIT licence, like Applio and RVC.
"""
import json, os, sys
import torch

sys.path.insert(0, os.getcwd())


class SynthForSudz(torch.nn.Module):
    """RVC inference as one graph: features + pitch in, audio out. `rnd` is the prior noise (zeros = smoothest)."""

    def __init__(self, net):
        super().__init__()
        self.net = net

    def forward(self, phone, phone_lengths, pitch, pitchf, sid, rnd):
        g = self.net.emb_g(sid).unsqueeze(-1)
        m_p, logs_p, x_mask = self.net.enc_p(phone, pitch, phone_lengths)
        z_p = (m_p + torch.exp(logs_p) * rnd * 0.66666) * x_mask
        z = self.net.flow(z_p, x_mask, g=g, reverse=True)
        return self.net.dec(z * x_mask, pitchf, g=g)


def _patch_for_export():
    # Applio's LayerNorm reads the channel count from the tensor, which the ONNX exporter can't trace; use the weights' size
    from rvc.lib.algorithm import normalization

    def forward(self, x):
        x = x.transpose(1, -1)
        x = torch.nn.functional.layer_norm(x, (self.gamma.shape[0],), self.gamma, self.beta, self.eps)
        return x.transpose(1, -1)

    normalization.LayerNorm.forward = forward


def export_voice(pth, out):
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
    m = SynthForSudz(net).eval()
    T = 200
    args = (torch.randn(1, T, 768), torch.tensor([T]).long(), torch.randint(1, 255, (1, T)).long(),
            torch.rand(1, T) * 300 + 100, torch.tensor([0]).long(), torch.zeros(1, 192, T))
    with torch.no_grad():
        torch.onnx.export(m, args, out, opset_version=17, do_constant_folding=True, dynamo=False,
                          input_names=["phone", "phone_lengths", "pitch", "pitchf", "sid", "rnd"], output_names=["audio"],
                          dynamic_axes={"phone": {1: "T"}, "pitch": {1: "T"}, "pitchf": {1: "T"}, "rnd": {2: "T"}, "audio": {2: "S"}})
    meta = {"sudz_voice": 1, "sr": int(cfg[-1]), "name": cpt.get("model_name", os.path.splitext(os.path.basename(pth))[0]),
            "epoch": cpt.get("epoch"), "speakers": int(cfg[-3]), "hop": int(cfg[-1]) // 100}
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
    export_voice(src, dst) if kind == "voice" else export_content(src, dst)
