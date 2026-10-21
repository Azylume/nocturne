#include "effects/isf.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <mutex>
#include <optional>
#include <regex>
#include <set>

namespace azl {

namespace {

using nlohmann::json;

struct Header {
    json meta;
    std::string glsl;
};

std::variant<Header, std::string> splitHeader(const std::string& text)
{
    const size_t open = text.find("/*");
    const size_t close = open == std::string::npos ? open : text.find("*/", open + 2);
    if (close == std::string::npos)
        return std::string("no JSON header comment");
    json meta = json::parse(text.begin() + long(open) + 2, text.begin() + long(close), nullptr, false);
    if (meta.is_discarded() || !meta.is_object())
        return std::string("the header is not a JSON object");
    return Header{std::move(meta), text.substr(close + 2)};
}

double number(const json& j, const char* key, double fallback)
{
    if (!j.contains(key))
        return fallback;
    const json& v = j[key];
    if (v.is_boolean())
        return v.get<bool>() ? 1.0 : 0.0;
    return v.is_number() ? v.get<double>() : fallback;
}

double component(const json& j, const char* key, size_t i, double fallback)
{
    if (!j.contains(key) || !j[key].is_array() || j[key].size() <= i || !j[key][i].is_number())
        return fallback;
    return j[key][i].get<double>();
}

std::string labelOf(const json& input)
{
    return input.value("LABEL", std::string());
}

std::string suffixed(const std::string& label, const char* suffix)
{
    return label.empty() ? label : label + " " + suffix;
}

void addFloat(const json& input, const std::string& name, IsfShader& shader)
{
    const double min = number(input, "MIN", 0.0);
    const double max = number(input, "MAX", 1.0);
    if (!(max > min))
        throw std::string("input " + name + " has MAX not above MIN");
    shader.params.push_back(
        {.name = name, .min = min, .max = max, .defaultValue = std::clamp(number(input, "DEFAULT", min), min, max),
         .label = labelOf(input)});
    shader.inputs.push_back({name, IsfInputType::Float});
}

void addBool(const json& input, const std::string& name, IsfShader& shader)
{
    shader.params.push_back({.name = name, .defaultValue = number(input, "DEFAULT", 0.0) >= 0.5 ? 1.0 : 0.0,
                             .label = labelOf(input), .choice = true, .options = {"Off", "On"}});
    shader.inputs.push_back({name, IsfInputType::Bool});
}

std::vector<int> longValues(const json& input, const std::string& name)
{
    std::vector<int> values;
    if (input.contains("VALUES") && input["VALUES"].is_array()) {
        for (const json& v : input["VALUES"])
            if (v.is_number())
                values.push_back(int(v.get<double>()));
    } else if (input.contains("MIN") && input.contains("MAX")) {
        for (int v = int(number(input, "MIN", 0.0)); v <= int(number(input, "MAX", 0.0)); ++v)
            values.push_back(v);
    }
    if (values.size() < 2)
        throw std::string("input " + name + " offers fewer than two values");
    return values;
}

void addLong(const json& input, const std::string& name, IsfShader& shader)
{
    const std::vector<int> values = longValues(input, name);
    std::vector<std::string> labels;
    for (size_t i = 0; i < values.size(); ++i) {
        const bool labelled = input.contains("LABELS") && input["LABELS"].is_array() && i < input["LABELS"].size() &&
                              input["LABELS"][i].is_string();
        labels.push_back(labelled ? input["LABELS"][i].get<std::string>() : std::to_string(values[i]));
    }
    const int fallback = int(number(input, "DEFAULT", values[0]));
    const auto at = std::find(values.begin(), values.end(), fallback);
    const double index = at == values.end() ? 0.0 : double(at - values.begin());
    shader.params.push_back({.name = name, .min = 0.0, .max = double(values.size() - 1), .defaultValue = index,
                             .label = labelOf(input), .choice = true, .options = std::move(labels)});
    shader.inputs.push_back({name, IsfInputType::Long, values});
}

void addColour(const json& input, const std::string& name, IsfShader& shader)
{
    const char* channels[] = {"Red", "Green", "Blue", "Alpha"};
    for (size_t i = 0; i < 4; ++i)
        shader.params.push_back({.name = name + channels[i],
                                 .defaultValue = std::clamp(component(input, "DEFAULT", i, i == 3 ? 1.0 : 0.0), 0.0, 1.0),
                                 .label = suffixed(labelOf(input), channels[i])});
    shader.inputs.push_back({name, IsfInputType::Colour});
}

void addPoint(const json& input, const std::string& name, IsfShader& shader)
{
    if (!input.contains("MIN") || !input.contains("MAX"))
        throw std::string("point input " + name + " needs MIN and MAX");
    const char* axes[] = {"X", "Y"};
    for (size_t i = 0; i < 2; ++i) {
        const double min = component(input, "MIN", i, 0.0);
        const double max = component(input, "MAX", i, 1.0);
        if (!(max > min))
            throw std::string("input " + name + " has MAX not above MIN");
        shader.params.push_back({.name = name + axes[i], .min = min, .max = max,
                                 .defaultValue = std::clamp(component(input, "DEFAULT", i, min), min, max),
                                 .label = suffixed(labelOf(input), axes[i])});
    }
    shader.inputs.push_back({name, IsfInputType::Point});
}

// The pictures an ISF kind reads, which are not parameters.
bool pictureInput(IsfKind kind, const std::string& name)
{
    if (kind == IsfKind::Transition)
        return name == "startImage" || name == "endImage";
    return kind == IsfKind::Effect && name == "inputImage";
}

void addInput(const json& input, IsfShader& shader)
{
    const std::string name = input.value("NAME", std::string());
    const std::string type = input.value("TYPE", std::string());
    if (name.empty())
        throw std::string("an input has no NAME");
    if (pictureInput(shader.kind, name) || (shader.kind == IsfKind::Transition && name == "progress"))
        return;
    if (type == "float")
        addFloat(input, name, shader);
    else if (type == "bool" || type == "event")
        addBool(input, name, shader);
    else if (type == "long")
        addLong(input, name, shader);
    else if (type == "color")
        addColour(input, name, shader);
    else if (type == "point2D")
        addPoint(input, name, shader);
    else
        throw std::string("input " + name + " of type " + type + " is not supported");
}

IsfKind kindOf(const json& inputs)
{
    std::set<std::string> images;
    for (const json& input : inputs)
        if (input.value("TYPE", std::string()) == "image")
            images.insert(input.value("NAME", std::string()));
    if (images.count("startImage") && images.count("endImage"))
        return IsfKind::Transition;
    return images.count("inputImage") ? IsfKind::Effect : IsfKind::Generator;
}

size_t count(const std::string& text, const std::regex& pattern)
{
    return size_t(std::distance(std::sregex_iterator(text.begin(), text.end(), pattern), std::sregex_iterator()));
}

bool effectPointwise(const std::string& glsl)
{
    static const std::regex any(R"(\binputImage\b)");
    static const std::regex here(
        R"(IMG_THIS_(NORM_)?PIXEL\s*\(\s*inputImage\s*\)|IMG_NORM_PIXEL\s*\(\s*inputImage\s*,\s*(isf|vv)_FragNormCoord(\.xy)?\s*\))");
    return count(glsl, any) == count(glsl, here);
}

// Every call of getFromColor and getToColor passes transition()'s own argument unchanged, and main() passes it the
// pixel's coordinate.
bool transitionPointwise(const std::string& glsl)
{
    std::smatch m;
    static const std::regex transition(R"(vec4\s+transition\s*\(\s*vec2\s+(\w+)\s*\))");
    if (!std::regex_search(glsl, m, transition))
        return false;
    const std::string arg = m[1];
    if (std::regex_search(glsl, std::regex("\\b" + arg + "\\s*([-+*/]?=[^=]|\\+\\+|--)")))
        return false;
    static const std::regex call(R"(get(From|To)Color\s*\(\s*([^()]*?)\s*\))");
    static const std::regex definition(R"(vec4\s+get(From|To)Color\s*\(\s*vec2\s+\w+\s*\))");
    size_t calls = 0;
    for (auto it = std::sregex_iterator(glsl.begin(), glsl.end(), call); it != std::sregex_iterator(); ++it, ++calls)
        if ((*it)[2] != arg && !std::regex_match((*it)[2].str(), std::regex(R"(vec2\s+\w+)")))
            return false;
    static const std::regex pictures(R"(\b(startImage|endImage)\b)");
    static const std::regex main(R"(gl_FragColor\s*=\s*transition\s*\(\s*isf_FragNormCoord(\.xy)?\s*\))");
    return calls > count(glsl, definition) && count(glsl, pictures) == 2 && count(glsl, definition) == 2 &&
           std::regex_search(glsl, main);
}

void checkNames(const IsfShader& shader)
{
    std::set<std::string> names{"mix"};
    for (const ParamDef& param : shader.params)
        if (!names.insert(param.name).second)
            throw std::string("parameter name " + param.name + " is taken");
}

// The ISF transitions built into the program, as modes of their own.
constexpr std::array<std::pair<TransitionMode, const char*>, 3> kBuiltinTransitions{{
    {TransitionMode::WindowSlice, "Window Slice"},
    {TransitionMode::Radial, "Radial"},
    {TransitionMode::RandomSquares, "Random Squares"},
}};

std::optional<TransitionMode> builtinMode(const std::string& name)
{
    for (const auto& [mode, n] : kBuiltinTransitions)
        if (name == n)
            return mode;
    return std::nullopt;
}

std::string typeOf(const IsfShader& shader)
{
    if (shader.kind == IsfKind::Transition && !shader.user)
        if (const auto mode = builtinMode(shader.name))
            return transitionName(*mode);
    return shader.user ? userIsfType(shader.name) : isfType(shader.name);
}

struct TransitionSet {
    std::mutex mutex;
    std::shared_ptr<const IsfTransitions> current;
};

TransitionSet& transitionSet()
{
    static TransitionSet set{.current = std::make_shared<const IsfTransitions>(builtinIsfTransitions())};
    return set;
}

void checkSupported(const json& meta)
{
    if (meta.contains("PASSES") && meta["PASSES"].is_array() &&
        (meta["PASSES"].size() > 1 || (meta["PASSES"].size() == 1 && meta["PASSES"][0].contains("TARGET"))))
        throw std::string("multi-pass shaders are not supported");
    if (meta.contains("IMPORTED") && !meta["IMPORTED"].empty())
        throw std::string("imported images are not supported");
}

} // namespace

std::string isfType(const std::string& name)
{
    std::string type = "isf";
    std::string word;
    auto flush = [&] {
        if (word.empty())
            return;
        const bool upper = std::none_of(word.begin(), word.end(), [](unsigned char c) { return std::islower(c); });
        type += char(std::toupper(static_cast<unsigned char>(word[0])));
        for (size_t i = 1; i < word.size(); ++i)
            type += upper ? char(std::tolower(static_cast<unsigned char>(word[i]))) : word[i];
        word.clear();
    };
    for (const char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)))
            word += c;
        else
            flush();
    }
    flush();
    return type;
}

std::string userIsfType(const std::string& name)
{
    const std::string type = isfType(name);
    return type.size() > 3 ? "user" + type.substr(3) : std::string();
}

std::variant<IsfShader, std::string> parseIsf(const IsfSource& source)
{
    auto split = splitHeader(source.text);
    if (auto* error = std::get_if<std::string>(&split))
        return *error;
    Header& header = std::get<Header>(split);
    const json inputs = header.meta.contains("INPUTS") && header.meta["INPUTS"].is_array() ? header.meta["INPUTS"]
                                                                                           : json::array();
    IsfShader shader{.name = source.name, .user = source.user, .kind = kindOf(inputs), .glsl = std::move(header.glsl)};
    shader.type = typeOf(shader);
    if (shader.type.empty())
        return std::string("the file name has no letters or digits");
    try {
        checkSupported(header.meta);
        for (const json& input : inputs)
            addInput(input, shader);
        checkNames(shader);
    } catch (const std::string& error) {
        return error;
    }
    if (shader.kind == IsfKind::Transition && shader.params.size() > kMaxTransitionParams)
        return "a transition takes at most " + std::to_string(kMaxTransitionParams) + " parameter values";
    if (shader.kind == IsfKind::Effect)
        shader.pointwise = effectPointwise(shader.glsl);
    if (shader.kind == IsfKind::Transition) {
        shader.pointwise = transitionPointwise(shader.glsl);
        if (!shader.pointwise)
            return std::string("a transition must read both pictures at its own pixel only");
    }
    return shader;
}

EffectDef isfEffect(const IsfShader& shader)
{
    const bool pointwise = shader.pointwise || shader.kind == IsfKind::Generator;
    EffectDef def{shader.type, pointwise ? StageKind::Pointwise : StageKind::Neighbourhood, shader.params, false, ""};
    def.category = shader.user ? "User" : "ISF";
    def.label = shader.name;
    def.generator = shader.kind == IsfKind::Generator;
    return def;
}

const std::vector<std::variant<IsfShader, std::string>>& parsedIsfSources()
{
    static const std::vector<std::variant<IsfShader, std::string>> parsed = [] {
        std::vector<std::variant<IsfShader, std::string>> out;
        for (const IsfSource& source : isfSources())
            out.push_back(parseIsf(source));
        return out;
    }();
    return parsed;
}

std::vector<EffectDef> isfEffects()
{
    std::vector<EffectDef> effects;
    for (const auto& parsed : parsedIsfSources()) {
        const auto* shader = std::get_if<IsfShader>(&parsed);
        if (shader && shader->kind != IsfKind::Transition)
            effects.push_back(isfEffect(*shader));
    }
    return effects;
}

IsfTransitions builtinIsfTransitions()
{
    IsfTransitions transitions;
    for (const auto& parsed : parsedIsfSources()) {
        const auto* shader = std::get_if<IsfShader>(&parsed);
        if (shader && shader->kind == IsfKind::Transition && builtinMode(shader->name))
            transitions.push_back(isfTransition(*shader));
    }
    return transitions;
}

IsfTransition isfTransition(const IsfShader& shader)
{
    const auto builtin = shader.user ? std::nullopt : builtinMode(shader.name);
    return {builtin ? *builtin : userTransitionMode(shader.type), shader.name, shader.params, shader.user};
}

std::shared_ptr<const IsfTransitions> isfTransitions()
{
    TransitionSet& set = transitionSet();
    std::lock_guard lock(set.mutex);
    return set.current;
}

void setIsfTransitions(IsfTransitions transitions)
{
    auto next = std::make_shared<const IsfTransitions>(std::move(transitions));
    TransitionSet& set = transitionSet();
    std::lock_guard lock(set.mutex);
    set.current.swap(next);
}

const IsfTransition* findIsfTransition(const IsfTransitions& transitions, TransitionMode mode)
{
    auto it = std::find_if(transitions.begin(), transitions.end(), [&](const IsfTransition& t) { return t.mode == mode; });
    return it == transitions.end() ? nullptr : &*it;
}

void mirrorUserIsf(const std::vector<IsfSource>& sources)
{
    static std::mutex mutex;
    static std::map<std::string, std::string> effects;
    static std::vector<TransitionMode> modes;
    std::lock_guard lock(mutex);
    std::map<std::string, std::string> registered;
    IsfTransitions transitions = *isfTransitions();
    std::erase_if(transitions, [&](const IsfTransition& t) { return std::ranges::count(modes, t.mode) > 0; });
    modes.clear();
    for (const IsfSource& source : sources) {
        const auto parsed = parseIsf(source);
        const auto* shader = std::get_if<IsfShader>(&parsed);
        if (!shader)
            continue;
        if (shader->kind == IsfKind::Transition) {
            IsfTransition transition = isfTransition(*shader);
            if (const IsfTransition* own = findIsfTransition(transitions, transition.mode); own && !own->function.empty())
                continue;
            modes.push_back(transition.mode);
            transitions.push_back(std::move(transition));
            continue;
        }
        const EffectDef* own = findEffect(shader->type);
        if (own && !own->metal.empty() && !effects.contains(shader->type))
            continue;
        if (!effects.contains(shader->type) || effects.at(shader->type) != source.text)
            registerEffect(isfEffect(*shader));
        registered[shader->type] = source.text;
    }
    for (const auto& [type, text] : effects)
        if (!registered.contains(type))
            retireEffect(type);
    effects = std::move(registered);
    setIsfTransitions(std::move(transitions));
}

std::vector<float> transitionParamValues(const IsfTransition& transition, const std::map<std::string, double>& values)
{
    std::vector<float> out;
    for (const ParamDef& param : transition.params) {
        auto it = values.find(param.name);
        out.push_back(float(it == values.end() ? param.defaultValue : std::clamp(it->second, param.min, param.max)));
    }
    return out;
}

} // namespace azl
