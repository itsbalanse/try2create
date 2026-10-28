# Try2Create's Drama 3 through audio.cpp: the same scene as the ComfyUI-Drama3 nodes, made by audio.cpp's native server from a GGUF
# (Jmica/Drama-3-GGUF, Q3 experts / Q8 backbone, 18.9 GiB: on a 24 GB card it runs wholly on the GPU). ComfyUI itself loads nothing:
# this node starts audiocpp_server.exe once (its settings in drama3gguf.json next to this file, written by Try2Create's set-up), sends
# the script and the voices, and returns the audio. Before it generates, ComfyUI's own models leave the GPU; after it, when the next
# queued prompt is not a Drama one, audio.cpp lets its model go so the GPU is free again.
import json
import os
import re
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

import torch

import comfy.model_management as mm

HERE = os.path.dirname(os.path.abspath(__file__))
SETTINGS = os.path.join(HERE, 'drama3gguf.json')
MODEL_ID = 'drama-3'
_server = None


def _settings():
    with open(SETTINGS, encoding='utf-8') as f:
        return json.load(f)


def _url(s, path):
    return f"http://127.0.0.1:{int(s.get('port', 8035))}{path}"


def _alive(s):
    try:
        with urllib.request.urlopen(_url(s, '/health'), timeout=2) as r:
            return r.status == 200
    except Exception:
        return False


def _start(s):
    global _server
    if _alive(s):
        return
    exe = s['exe']
    if not os.path.isfile(exe):
        raise RuntimeError(f'audiocpp_server.exe was not found at {exe} (Drama page > Set up)')
    if not os.path.isfile(s['model']):
        raise RuntimeError(f"The Drama 3 GGUF was not found at {s['model']} (Drama page > Set up)")
    config = {
        'host': '127.0.0.1', 'port': int(s.get('port', 8035)), 'backend': 'cuda', 'threads': int(s.get('threads', 8)),
        'lazy_load': True, 'max_loaded_models': 1, 'idle_unload_ms': int(s.get('idle_unload_ms', 180000)), 'ui': False,
        'models': [{
            'id': MODEL_ID, 'family': 'fish_drama3', 'path': s['model'], 'task': 'tts', 'mode': 'offline',
            'session_options': s.get('session_options', {
                'fish_drama3.cpu_expert_layers': 0,
                'fish_drama3.context_length': 4096,
            }),
            'default_request_options': {'max_tokens': 1024},
        }],
    }
    cfg_path = os.path.join(tempfile.gettempdir(), 'try2create_drama3gguf_server.json')
    with open(cfg_path, 'w', encoding='utf-8') as f:
        json.dump(config, f, indent=1)
    log = open(os.path.join(tempfile.gettempdir(), 'try2create_drama3gguf_server.log'), 'w', encoding='utf-8', errors='replace')
    flags = 0x08000000 if os.name == 'nt' else 0  # CREATE_NO_WINDOW
    _server = subprocess.Popen([exe, '--config', cfg_path], cwd=os.path.dirname(exe), stdout=log, stderr=subprocess.STDOUT, creationflags=flags)
    for _ in range(240):  # up to 2 min (a lazy server answers before the model loads)
        if _alive(s):
            return
        if _server.poll() is not None:
            raise RuntimeError('audiocpp_server stopped while starting; see try2create_drama3gguf_server.log in the temp folder')
        time.sleep(0.5)
    raise RuntimeError('audiocpp_server did not answer')


def _post(s, path, body, timeout):
    req = urllib.request.Request(_url(s, path), json.dumps(body).encode('utf-8'), {'Content-Type': 'application/json'})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        raise RuntimeError(f'audio.cpp: {e.code} {e.read()[:600].decode("utf-8", "replace")}')


def _wav_seconds(data):
    """The length of a WAV audio.cpp sent (its data chunk over its byte rate); 0 when it cannot be read."""
    try:
        rate = int.from_bytes(data[28:32], 'little')
        at = data.find(b'data', 12)
        return int.from_bytes(data[at + 4:at + 8], 'little') / rate if at > 0 and rate > 0 else 0.0
    except Exception:
        return 0.0


def _lock_voices(s, text, refs, seed, options):
    """A voice for every speaker without one, so it stays the same all through the scene.

    audio.cpp makes a long scene in chunks of about one or two lines, and each chunk hears only the chunk before: a speaker who was not in it
    got a new invented voice (a test on a two-person, 14-line scene: voices changed every line or two, a woman's lines now and then in a man's
    voice). Each such speaker first says their own first line alone (with its description cue), and that clip goes along as their reference,
    the way a kept voice does - in a test, the steadiest of three ways (a casting take as history was steadier than nothing, these clips more).
    Returns the references with the new ones added."""
    have = set(re.findall(r'<\|speaker:(\d+)\|>', refs or ''))
    turns = re.findall(r'<\|speaker:(\d+)\|>\s*(.*?)(?=<\|speaker:\d+\|>|$)', text, flags=re.S)
    first = {}
    for n, said in turns:
        if n in have:
            continue
        said = ' '.join(said.split())
        if n not in first:
            first[n] = said
        elif len(re.sub(r'\[[^\]]*\]|<[^>]*>', '', first[n]).strip()) < 40:  # a very short first line: the next one too, for enough voice
            first[n] += ' ' + re.sub(r'^\s*(\[[^\]]*\]\s*)+', '', said)
    out = json.loads(refs) if refs else []
    for k, (n, said) in enumerate(first.items()):
        spoken = re.sub(r'\[[^\]]*\]|<[^>]*>', '', said)  # what the clip says: its transcript without the cues
        spoken = ' '.join(spoken.split())
        if not spoken:
            continue
        clip = None
        for attempt in range(2):  # a clip that fell silent or ran on is made again once
            o = dict(options, text_chunk_size=100000, max_tokens=min(800, 120 + len(spoken) * 3))
            o.pop('references', None)
            body = {'model': MODEL_ID, 'input': f'<|speaker:{n}|> {said}', 'seed': str((int(seed) + 7919 * (k + 1) + 104729 * attempt) % 4294967296), 'options': o}
            wav = _post(s, '/v1/audio/speech', body, timeout=600)
            secs = _wav_seconds(wav)
            if 0.8 <= secs <= max(4.0, len(spoken.split()) * 1.2):
                clip = wav
                break
            clip = clip or wav
        path = os.path.join(tempfile.gettempdir(), f'try2create_drama3gguf_voice{n}_{os.getpid()}.wav')
        with open(path, 'wb') as f:
            f.write(clip)
        out.append({'text': f'<|speaker:{n}|> {spoken}', 'audio_path': path.replace('\\', '/')})
    return json.dumps(out) if out else ''


def _next_is_drama():
    """Is a Drama prompt waiting in ComfyUI's queue? Then audio.cpp keeps its model loaded for it."""
    try:
        import server
        _, pending = server.PromptServer.instance.prompt_queue.get_current_queue()
        for item in pending:
            graph = item[2] if len(item) > 2 else {}
            if any(isinstance(n, dict) and n.get('class_type') == 'Drama3GGUFGenerate' for n in graph.values()):
                return True
    except Exception:
        pass
    return False


class Drama3GGUFGenerate:
    @classmethod
    def INPUT_TYPES(cls):
        return {
            'required': {
                'text': ('STRING', {'multiline': True, 'default': ''}),
                'seed': ('INT', {'default': 0, 'min': 0, 'max': 2**53}),
                'temperature': ('FLOAT', {'default': 1.0, 'min': 0.0, 'max': 2.0, 'step': 0.01}),
                'top_p': ('FLOAT', {'default': 0.9, 'min': 0.0, 'max': 1.0, 'step': 0.01}),
                'top_k': ('INT', {'default': 4097, 'min': 1, 'max': 4097}),
                'max_tokens': ('INT', {'default': 1024, 'min': 16, 'max': 16384, 'tooltip': 'Frames per text chunk (about 21.5 a second)'}),
                'text_chunk_size': ('INT', {'default': 128, 'min': 16, 'max': 100000, 'tooltip': 'Characters per chunk; a large number keeps the script in one chunk'}),
            },
            'optional': {
                'references': ('STRING', {'multiline': True, 'default': '',
                                          'tooltip': 'JSON: [{"text": "<|speaker:0|> what the clip says", "audio_path": "C:/clip.wav"}, ...]'}),
                'lock_voices': ('BOOLEAN', {'default': True,
                                            'tooltip': 'Give every speaker without a reference a voice first (their first line alone), so it holds all through the scene'}),
            },
        }

    RETURN_TYPES = ('AUDIO',)
    FUNCTION = 'generate'
    CATEGORY = 'audio/Drama3'

    def generate(self, text, seed, temperature, top_p, top_k, max_tokens, text_chunk_size, references='', lock_voices=True):
        s = _settings()
        _start(s)
        mm.unload_all_models()  # (ComfyUI's models off the GPU: audio.cpp needs most of it)
        mm.soft_empty_cache()
        options = {'temperature': temperature, 'top_p': top_p, 'top_k': top_k, 'max_tokens': max_tokens, 'text_chunk_size': text_chunk_size}
        refs = (references or '').strip()
        if refs == '[]':
            refs = ''
        if refs:
            json.loads(refs)  # (a broken list fails here, with Python's message)
        if lock_voices:
            refs = _lock_voices(s, text, refs, seed, options)
        if refs:
            options['references'] = refs
        body = {'model': MODEL_ID, 'input': text, 'seed': str(int(seed) % 4294967296), 'options': options}  # (its Windows build reads a 32-bit seed)
        wav = _post(s, '/v1/audio/speech', body, timeout=3600)
        if not _next_is_drama():
            try:
                _post(s, '/v1/tasks/unload_all_models', {}, timeout=120)
            except Exception:
                pass
        path = os.path.join(tempfile.gettempdir(), f'try2create_drama3gguf_{os.getpid()}.wav')
        with open(path, 'wb') as f:
            f.write(wav)
        from comfy_extras.nodes_audio import load
        waveform, rate = load(path)
        try:
            os.remove(path)
        except OSError:
            pass
        return ({'waveform': waveform.unsqueeze(0) if waveform.dim() == 2 else waveform, 'sample_rate': rate},)


NODE_CLASS_MAPPINGS = {'Drama3GGUFGenerate': Drama3GGUFGenerate}
NODE_DISPLAY_NAME_MAPPINGS = {'Drama3GGUFGenerate': 'Drama 3 (audio.cpp GGUF)'}
