#pragma once
// Lyrics in a song's own file: the words (an MP3's USLT frame, which phones and music players show), and - when a whisper.cpp model is there -
// each line timed to the singing: an SYLT frame (synced lyrics), a .lrc file next to the song (VLC, MusicBee, foobar2000, Poweramp...) and
// a .srt (subtitles: the song's video for posting shows them). ffmpeg's own whisper filter listens to the song; what it hears is matched
// word by word to the lyrics that were written, so the lines are the real ones, only the times come from listening.
#include <string>
#include <vector>

struct TimedLyric {
    double start = 0, end = 0; // seconds
    std::string text;
};

std::vector<std::string> SungLyricLines(const std::string& lyrics); // the sung lines: no [section] tags, no empty lines
std::string FitLyrics(const std::string& lyrics, double seconds);   // the sections that fit the length (the song stops at max_duration)
std::wstring WhisperModelPath();                                     // the whisper.cpp model (models\whisper\ggml-*.bin), "" = none
const char* WhisperModelUrl();                                       // the one to get (ggml-small, 488 MB)
std::wstring WhisperModelDir();                                      // where it goes
// The lyrics pass on a finished MP3 (blocks: a worker thread). Returns what was done ("timed 18 lines", "lyrics without timing: ...").
std::string AddLyricsToSong(const std::wstring& song, const std::string& lyrics, const std::string& title);
std::string LyricsFromSongFile(const std::wstring& song);            // the lyrics ComfyUI saved in the song's workflow ("" = none)
std::vector<TimedLyric> ReadSrt(const std::wstring& srt);
// Any song's words from listening alone, when there are no lyrics to match them to: whisper's own timed lines into outSrt. Blocks (a worker
// thread; about a minute for a 3-minute song). "" = done, else why not.
std::string HearSongLyrics(const std::wstring& song, const std::wstring& outSrt);
std::wstring SrtPathOf(const std::wstring& song);                    // song.srt (exists once timed)
// A rewritten song (tmp) in place of the song: tried for up to 10 s while a player still has it open; the tmp is removed when it cannot be done
bool ReplaceSongFile(const std::wstring& tmp, const std::wstring& song);
