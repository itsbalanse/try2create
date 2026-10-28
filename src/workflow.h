#pragma once
// Workflows embedded in images (ComfyUI node graphs and the generation parameters of other tools):
// finding them, parsing ComfyUI graphs for the node viewer, and the on-disk workflow library.
#include <atomic>
#include <cstdint>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <d3d11.h>
#include "metadata.h"

enum class WorkflowKind {
    ComfyUI,   // the editable node graph ("workflow") - what ComfyUI loads when an image is dropped on it
    ComfyApi,  // the API-format prompt ("prompt"): the same graph without layout
    InvokeAI,
    NovelAI,
    Parameters // AUTOMATIC1111-style "parameters" text
};

struct EmbeddedWorkflow {
    WorkflowKind kind = WorkflowKind::ComfyUI;
    const char* label = "";            // e.g. "ComfyUI workflow"
    const std::string* text = nullptr; // points into the ImageMetadata it was found in
    bool Viewable() const { return kind == WorkflowKind::ComfyUI || kind == WorkflowKind::ComfyApi; }
};

const char* WorkflowKindLabel(WorkflowKind kind);
inline unsigned WorkflowBit(WorkflowKind kind) { return 1u << static_cast<int>(kind); }
// Kinds a viewer can draw (the others are only copied).
inline unsigned ViewableWorkflowMask() { return WorkflowBit(WorkflowKind::ComfyUI) | WorkflowBit(WorkflowKind::ComfyApi); }

// Everything workflow-like in the metadata, best first (ComfyUI graph, ComfyUI API prompt, then the others).
std::vector<EmbeddedWorkflow> FindWorkflows(const ImageMetadata& meta);
// The graph to show in the node viewer (ComfyUI workflow, else the API prompt); nullptr if there is none.
const EmbeddedWorkflow* FindViewableWorkflow(const std::vector<EmbeddedWorkflow>& found);

// ---------------------------------------------------------------------------
// ComfyUI graph model
// ---------------------------------------------------------------------------
struct WfPort {
    std::string name, type;
    int link = -1; // inputs: id of the connected link
};

struct WfWidget {
    std::string name, value;
    bool multiline = false; // long text such as a prompt
};

struct WfNode {
    int id = 0;
    std::string idText; // as written in the file
    std::string type, title;
    std::string subgraphId; // non-empty: this node is a subgraph; double-click opens its inside
    float x = 0, y = 0, w = 200, h = 80; // body rectangle; the title bar sits above y
    int mode = 0;                        // 0 normal, 2 muted, 4 bypassed
    bool collapsed = false;
    bool hasColor = false, hasBg = false;
    uint32_t color = 0, bgcolor = 0; // 0xRRGGBB
    std::vector<WfPort> inputs, outputs;
    std::vector<WfWidget> widgets;
};

struct WfLink {
    int id = 0;
    int from = 0, fromSlot = 0, to = 0, toSlot = 0; // node ids
    std::string type;
};

struct WfGroup {
    std::string title;
    float x = 0, y = 0, w = 0, h = 0;
    bool hasColor = false;
    uint32_t color = 0;
};

struct WfGraph {
    std::vector<WfNode> nodes;
    std::vector<WfLink> links;
    std::vector<WfGroup> groups;
    std::unordered_map<int, int> indexById; // node id -> index in nodes
    bool fromApi = false;                   // laid out automatically (the file has no positions)
    std::string name;                       // subgraph name when this is the inside of one
    int hiddenNodes = 0;                    // nodes inside subgraphs (not drawn at this level)
    std::vector<std::pair<std::string, WfWidget>> deep; // (node title, widget) of everything inside subgraphs
    float minX = 0, minY = 0, maxX = 0, maxY = 0;
    void Clear() { *this = WfGraph(); }
    const WfNode* Find(int id) const {
        auto it = indexById.find(id);
        return it == indexById.end() ? nullptr : &nodes[it->second];
    }
};

// Parses a ComfyUI workflow (editable graph or API prompt). When both the graph and the API prompt
// of the same image are known, pass the latter as `apiPrompt` to get the widget names.
// `path` (optional) descends into subgraphs: (subgraph id, id of the node that holds it) per level.
using WfSubPath = std::vector<std::pair<std::string, int>>;
bool ParseWorkflowGraph(const std::string& text, const std::string* apiPrompt, WfGraph& out, std::string* error,
                        const WfSubPath* path = nullptr);

// "42 nodes - model.safetensors - 2 LoRAs" for list rows.
std::string SummarizeWorkflow(const WfGraph& graph);

// ---------------------------------------------------------------------------
// Library (the program's workflow cache folder)
// ---------------------------------------------------------------------------
struct WorkflowLibItem {
    std::wstring jsonPath, thumbPath;
    std::string name, kindLabel, summary, source; // source = path of the image it came from
    int nodes = 0;
    long long saved = 0; // unix seconds
    bool viewable = true;
};

std::wstring WorkflowLibraryDir();
std::vector<WorkflowLibItem> ScanWorkflowLibrary();
// Writes the workflow (and a thumbnail of the image) into the library. Returns the saved .json path,
// or empty on failure. `existed` is set when an identical workflow from that image was already there.
std::wstring SaveWorkflowToLibrary(ID3D11Device* device, const std::wstring& imagePath, const ImageMetadata& meta,
                                   bool* existed, std::string* error);
// Saves a workflow that does not come from an image (opened from a .json file).
std::wstring SaveWorkflowTextToLibrary(const std::string& name, const std::string& text, bool* existed, std::string* error);
bool DeleteWorkflowFromLibrary(const WorkflowLibItem& item);

// Reads a workflow .json file, or the workflow of an image file; empty `kindOut` + empty text if none.
bool ReadWorkflowFromFile(const std::wstring& path, std::string& textOut, std::string& apiOut, std::string* error);

// Background "extract the workflows of these images into the library" job.
class WorkflowBatch {
public:
    ~WorkflowBatch() { Stop(); }
    void Start(ID3D11Device* device, std::vector<std::wstring> images);
    void Stop();
    bool Running() const { return running_.load(); }
    int Done() const { return done_.load(); }
    int Total() const { return total_; }
    // True once after the job finished; fills the summary line.
    bool TakeResult(std::string& message);

private:
    std::thread thread_;
    std::atomic<bool> running_{false}, cancel_{false}, finished_{false};
    std::atomic<int> done_{0};
    int total_ = 0;
    std::atomic<int> saved_{0}, existed_{0}, none_{0};
};

// What the "Workflows" tab is showing (owned by AppState, drawn by DrawWorkflowTab).
struct WorkflowViewState {
    // library list
    std::vector<WorkflowLibItem> lib;
    std::future<std::vector<WorkflowLibItem>> libFuture;
    bool libLoaded = false, libDirty = true, libLoading = false;
    int libSel = -1;
    // the open graph
    bool open = false;
    WfGraph graph;
    std::string text, api, title, kindLabel, error, summary;
    std::wstring sourceImage; // the image it came from, if any
    std::wstring libPath;     // its file in the library, if it is saved there
    bool viewFresh = true;    // fit it to the canvas on the next draw
    float panX = 0, panY = 0, zoom = 1;
    int selNode = -1;         // node id
    WfSubPath subPath;        // which subgraph is open (empty = the top level)
    std::vector<std::string> subNames;
    int centerOn = -1;        // node id to scroll to
    std::string nodeSearch, listSearch;
    float inspectorW = 330.0f;
    WorkflowBatch batch;
    std::string message;
};
