#include "driver_base.h"

#include "logger.h"
#include "cfg.h"

#include "video_base.h"
#include "buffered_audio.h"
#include "input_base.h"
#include "throttle.h"
#include "util.h"

#include <variables.h>
#include <core.h>

#include <cstring>
#include <cmath>
#include <memory>
#include <fstream>

namespace libretro {
extern struct retro_vfs_interface vfs_interface;
}

namespace drivers {

#ifdef _WIN32
#define PATH_SEPARATOR_CHAR "\\"
#else
#define PATH_SEPARATOR_CHAR "/"
#endif

inline void lowered_string(std::string &s) {
    for (char &c: s) {
        if (c <= ' ' || c == '\\' || c == '/' || c == ':' || c == '*' || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
        else
            c = std::tolower(c);
    }
}

inline std::string get_base_name(const std::string &path) {
    std::string basename = path;
    auto pos = basename.find_last_of("/\\");
    if (pos != std::string::npos) {
        basename = basename.substr(pos + 1);
    }
    pos = basename.find_last_of('.');
    if (pos != std::string::npos)
        basename.erase(pos);
    return basename;
}

driver_base *current_driver = nullptr;

driver_base::driver_base() {
    frame_throttle = std::make_shared<throttle>();
    variables = std::make_unique<libretro::retro_variables>();
}

driver_base::~driver_base() {
    deinit_internal();

    if (core) {
        core_unload(core);
        core = nullptr;
    }

    current_driver = nullptr;
}

void driver_base::set_dirs(const std::string &static_root, const std::string &config_root) {
    static_dir = static_root;
    config_dir = config_root;
    util_mkdir(config_dir.c_str());
    system_dir = config_root + PATH_SEPARATOR_CHAR "system";
    util_mkdir(system_dir.c_str());
    save_dir = config_root + PATH_SEPARATOR_CHAR "saves";
    util_mkdir(save_dir.c_str());
}

void driver_base::run(std::function<void()> in_game_menu_cb) {
    while (!shutdown_driver && run_frame(in_game_menu_cb, video->frame_drawn())) {
        auto check = g_cfg.get_save_check();
        if (check) {
            if (!save_check_countdown) {
                check_save_ram();
                save_check_countdown = lround(check * fps);
            } else {
                save_check_countdown--;
            }
        }
        core->retro_run();
        video->message_frame_pass();
    }
}

bool RETRO_CALLCONV retro_environment_cb(unsigned cmd, void *data) {
    if (!current_driver) return false;
    return current_driver->env_callback(cmd, data);
}

void RETRO_CALLCONV log_printf(enum retro_log_level level, const char *fmt, ...) {
#if defined(NDEBUG) || !defined(LIBRETRO_DEBUG_LOG)
    if (level >= RETRO_LOG_DEBUG)
        return;
#endif
    va_list l;
    va_start(l, fmt);
    log_vprintf((int)level, fmt, l);
    va_end(l);
}

static void RETRO_CALLCONV retro_video_refresh_cb(const void *data, unsigned width, unsigned height, size_t pitch) {
    if (!data) return;
    current_driver->get_video()->render(data, width, height, pitch);
}

static void RETRO_CALLCONV retro_audio_sample_cb(int16_t left, int16_t right) {
    int16_t samples[2] = {left, right};
    current_driver->get_audio()->write_samples(samples, 2);
}

static size_t RETRO_CALLCONV retro_audio_sample_batch_cb(const int16_t *data, size_t frames) {
    current_driver->get_audio()->write_samples(data, frames * 2);
    return frames;
}

static void RETRO_CALLCONV retro_input_poll_cb() {
    current_driver->get_input()->input_poll();
}

static int16_t RETRO_CALLCONV retro_input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id) {
    return current_driver->get_input()->input_state(port, device, index, id);
}

static bool RETRO_CALLCONV retro_set_rumble_state_cb(unsigned port, enum retro_rumble_effect effect, uint16_t strength) {
    return false;
}

inline bool read_file(const std::string filename, std::vector<uint8_t> &data) {
    std::ifstream ifs(filename, std::ios_base::binary | std::ios_base::in);
    if (!ifs.good()) return false;
    ifs.seekg(0, std::ios_base::end);
    size_t sz = ifs.tellg();
    if (!sz) {
        ifs.close();
        return false;
    }
    data.resize(sz);
    ifs.seekg(0, std::ios_base::beg);
    ifs.read((char *)data.data(), sz);
    ifs.close();
    return true;
}

bool driver_base::load_game(const std::string &path) {
    retro_game_info info = {};
    info.path = path.c_str();
    if (!need_fullpath) {
        std::ifstream ifs(path, std::ios_base::binary | std::ios_base::in);
        if (!ifs.good()) {
            logger(LOG_ERROR) << "Unable to load " << path << std::endl;
            return false;
        }
        ifs.seekg(0, std::ios_base::end);
        game_data.resize(ifs.tellg());
        ifs.seekg(0, std::ios_base::beg);
        ifs.read(&game_data[0], game_data.size());
        ifs.close();
        info.data = &game_data[0];
        info.size = game_data.size();
    }
    if (!core->retro_load_game(&info)) {
        logger(LOG_ERROR) << "Unable to load " << path << std::endl;
        return false;
    }

    game_path = path;
    post_load();
    return true;
}

bool driver_base::load_game_from_mem(const std::string &path, const std::string ext, const std::vector<uint8_t> &data) {
    retro_game_info info = {};
    if (!need_fullpath) {
        game_data.assign(data.begin(), data.end());
        info.path = path.c_str();
        info.data = &game_data[0];
        info.size = game_data.size();
    } else {
        std::string basename = get_base_name(path);
        temp_file = config_dir + PATH_SEPARATOR_CHAR "tmp";
        util_mkdir(temp_file.c_str());
        temp_file = temp_file + PATH_SEPARATOR_CHAR + basename + "." + ext;
        std::ofstream ofs(temp_file, std::ios_base::binary | std::ios_base::out | std::ios_base::trunc);
        if (!ofs.good()) return false;
        ofs.write((const char*)&data[0], data.size());
        if (ofs.bad()) {
            ofs.close();
            remove(temp_file.c_str());
            return false;
        }
        ofs.close();
        info.path = temp_file.c_str();
    }
    if (!core->retro_load_game(&info)) {
        logger(LOG_ERROR) << "Unable to load " << path << std::endl;
        return false;
    }

    game_path = path;
    post_load();
    return true;
}

void driver_base::unload_game() {
    shutdown_driver = false;
    check_save_ram();
    game_path.clear();
    game_base_name.clear();
    game_save_path.clear();
    game_rtc_path.clear();
    save_data.clear();
    rtc_data.clear();
    core->retro_unload_game();
    audio->stop();
    unload();
    if (!temp_file.empty()) {
        remove(temp_file.c_str());
        temp_file.clear();
    }
}

void driver_base::reset() {
    core->retro_reset();
}

bool driver_base::env_callback(unsigned cmd, void *data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_SET_ROTATION:
            break;
        case RETRO_ENVIRONMENT_GET_OVERSCAN:
            *(bool*)data = false;
            return true;
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *(bool*)data = true;
            return true;
        case RETRO_ENVIRONMENT_SET_MESSAGE: {
            const auto *msg = (const retro_message*)data;
            video->set_message(msg->msg, msg->frames);
            return true;
        }
        case RETRO_ENVIRONMENT_SHUTDOWN:
            shutdown_driver = true;
            return true;
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
            return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
            *(const char**)data = system_dir.c_str();
            return true;
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
            auto new_format = (unsigned)*(const enum retro_pixel_format *)data;
            if (new_format != pixel_format) {
                pixel_format = new_format;
                video->resolution_changed(base_width, base_height, pixel_format == RETRO_PIXEL_FORMAT_XRGB8888 ? 32 : 16);
            }
            return true;
        }
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS: {
            const auto *inp = (const retro_input_descriptor*)data;
            while (inp->description != nullptr) {
                input->add_button(inp->port, inp->device, inp->index, inp->id, inp->description);
                ++inp;
            }
            return true;
        }
        case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
        case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
        case RETRO_ENVIRONMENT_SET_HW_RENDER:
            break;
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            variables->set_variables_updated(false);
            auto *var = (retro_variable *)data;
            auto *vari = variables->get_variable(var->key);
            if (vari) {
                var->value = vari->options[vari->curr_index].first.c_str();
                return true;
            }
            return false;
        }
        case RETRO_ENVIRONMENT_SET_VARIABLES: {
            const auto *vars = (const retro_variable*)data;
            variables->load_variables(vars);
            variables->load_variables_from_cfg(core_cfg_path);
            return true;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            *(bool*)data = variables->get_variables_updated();
            return true;
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
            support_no_game = *(bool*)data;
            return true;
        case RETRO_ENVIRONMENT_GET_LIBRETRO_PATH:
            *(const char**)data = nullptr;
            return true;
        case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK:
        case RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK:
            break;
        case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE: {
            auto *ri = (retro_rumble_interface*)data;
            ri->set_rumble_state = retro_set_rumble_state_cb;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES:
            *(uint64_t*)data = (1ULL << RETRO_DEVICE_JOYPAD) | (1ULL << RETRO_DEVICE_ANALOG);
            return true;
        case RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE:
        case RETRO_ENVIRONMENT_GET_CAMERA_INTERFACE:
            break;
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
            ((retro_log_callback*)data)->log = log_printf;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_PERF_INTERFACE:
        case RETRO_ENVIRONMENT_GET_LOCATION_INTERFACE:
        case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
            break;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *(const char**)data = core_save_dir.empty() ? nullptr : core_save_dir.c_str();
            return true;
        case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        case RETRO_ENVIRONMENT_SET_PROC_ADDRESS_CALLBACK:
        case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
            break;
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO: {
            const auto *info = (const retro_controller_info*)data;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_MEMORY_MAPS: {
            const auto *memmap = (const retro_memory_map*)data;
            for (unsigned i = 0; i < memmap->num_descriptors; ++i) {
                /* TODO: store info of memory map for future use */
            }
            return true;
        }
        case RETRO_ENVIRONMENT_SET_GEOMETRY: {
            const auto *geometry = (const retro_game_geometry*)data;
            base_width = geometry->base_width;
            base_height = geometry->base_height;
            max_width = geometry->max_width;
            max_height = geometry->max_height;
            aspect_ratio = geometry->aspect_ratio;
            video->resolution_changed(base_width, base_height, pixel_format == RETRO_PIXEL_FORMAT_XRGB8888 ? 32 : 16);
            return true;
        }
        case RETRO_ENVIRONMENT_GET_USERNAME:
            *(const char**)data = "sdlretro";
            return true;
        case RETRO_ENVIRONMENT_GET_LANGUAGE:
            *(unsigned*)data = RETRO_LANGUAGE_ENGLISH;
            return true;
        case RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER:
            /*
        {
            auto *fb = (retro_framebuffer*)data;
            fb->data = video->get_framebuffer(&fb->width, &fb->height, &fb->pitch, (int*)&fb->format);
            if (fb->data)
                return true;
        }
             */
            return false;
        case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
            break;
        case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
            support_achivements = data ? *(bool*)data : true;
            return true;
        case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE:
        case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
        case RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT:
            break;
        case RETRO_ENVIRONMENT_GET_VFS_INTERFACE: {
            auto *info = (struct retro_vfs_interface_info *)data;
            if (info->required_interface_version > 3) return false;
            info->iface = &libretro::vfs_interface;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_LED_INTERFACE:
        case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
        case RETRO_ENVIRONMENT_GET_MIDI_INTERFACE:
        case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
        case RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE:
            break;
        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
            if (data) *(bool*)data = true;
            return true;
        case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
            *(unsigned*)data = RETRO_API_VERSION;
            return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS: {
            variables->load_variables((const retro_core_option_definition*)data);
            variables->load_variables_from_cfg(core_cfg_path);
            return true;
        }
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL: {
            variables->load_variables(((const retro_core_options_intl*)data)->us);
            variables->load_variables_from_cfg(core_cfg_path);
            return true;
        }
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY: {
            const auto *opt = (const retro_core_option_display*)data;
            variables->set_variable_visible(opt->key, opt->visible);
            return true;
        }
        case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
        default:
            break;
    }
    if (cmd & RETRO_ENVIRONMENT_EXPERIMENTAL) {
        logger(LOG_INFO) << "Unhandled env: " << (cmd & 0xFFFFU) << "(EXPERIMENTAL)" << std::endl;
    } else {
        logger(LOG_INFO) << "Unhandled env: " << cmd << std::endl;
    }
    return false;
}

void driver_base::save_variables_to_cfg() {
    variables->save_variables_to_cfg(core_cfg_path);
}

bool driver_base::load_core(const std::string &path) {
    core = core_load(path.c_str());
    if (!core) return false;

    current_driver = this;

    core_cfg_path = config_dir + PATH_SEPARATOR_CHAR + "cfg";
    util_mkdir(core_cfg_path.c_str());
    retro_system_info sysinfo = {};
    core->retro_get_system_info(&sysinfo);
    library_name = sysinfo.library_name;
    library_version = sysinfo.library_version;
    need_fullpath = sysinfo.need_fullpath;
    std::string name = sysinfo.library_name;
    lowered_string(name);
    core_cfg_path = core_cfg_path + PATH_SEPARATOR_CHAR + name + ".cfg";
    core_save_dir = save_dir + PATH_SEPARATOR_CHAR + name;
    util_mkdir(core_save_dir.c_str());

    init_internal();
    return true;
}

bool driver_base::init_internal() {
    if (inited) return true;

    if (!init()) {
        return false;
    }

    shutdown_driver = false;
    core->retro_set_environment(retro_environment_cb);

    core->retro_init();

    core->retro_set_video_refresh(retro_video_refresh_cb);
    core->retro_set_audio_sample(retro_audio_sample_cb);
    core->retro_set_audio_sample_batch(retro_audio_sample_batch_cb);
    core->retro_set_input_poll(retro_input_poll_cb);
    core->retro_set_input_state(retro_input_state_cb);

    inited = true;
    return true;
}

void driver_base::deinit_internal() {
    if (!inited) return;

    core->retro_deinit();

    /* reset all variables to default value */
    library_name.clear();
    library_version.clear();
    need_fullpath = false;

    pixel_format = 0;
    support_no_game = false;

    base_width = 0;
    base_height = 0;
    max_width = 0;
    max_height = 0;
    aspect_ratio = 0.f;

    game_data.clear();

    variables->reset();

    inited = false;
}

void driver_base::check_save_ram() {
    // TODO: use progressive check for large sram?
    size_t sram_size = core->retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (sram_size) {
        void *sram = core->retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
        if (sram_size != save_data.size() || memcmp(sram, save_data.data(), sram_size) != 0) {
            std::ofstream ofs(game_save_path, std::ios_base::binary | std::ios_base::out | std::ios_base::trunc);
            ofs.write((const char*)sram, sram_size);
            ofs.close();
            save_data.assign((uint8_t*)sram, (uint8_t*)sram + sram_size);
        }
    }
    size_t rtc_size = core->retro_get_memory_size(RETRO_MEMORY_RTC);
    if (rtc_size) {
        void *rtc = core->retro_get_memory_data(RETRO_MEMORY_RTC);
        if (rtc_size != rtc_data.size() || memcmp(rtc, rtc_data.data(), rtc_size) != 0) {
            std::ofstream ofs(game_rtc_path, std::ios_base::binary | std::ios_base::out | std::ios_base::trunc);
            ofs.write((const char*)rtc, rtc_size);
            ofs.close();
            rtc_data.assign((uint8_t*)rtc, (uint8_t*)rtc + rtc_size);
        }
    }
}

void driver_base::post_load() {
    game_base_name = get_base_name(game_path);
    game_save_path = (core_save_dir.empty() ? "" : (core_save_dir + PATH_SEPARATOR_CHAR)) + game_base_name + ".sav";
    game_rtc_path = (core_save_dir.empty() ? "" : (core_save_dir + PATH_SEPARATOR_CHAR)) + game_base_name + ".rtc";

    read_file(game_save_path, save_data);
    read_file(game_rtc_path, rtc_data);

    if (!save_data.empty()) {
        size_t sz = core->retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
        if (sz > save_data.size()) sz = save_data.size();
        if (sz) memcpy(core->retro_get_memory_data(RETRO_MEMORY_SAVE_RAM), save_data.data(), sz);
    }
    if (!rtc_data.empty()) {
        size_t sz = core->retro_get_memory_size(RETRO_MEMORY_RTC);
        if (sz > rtc_data.size()) sz = rtc_data.size();
        if (sz) memcpy(core->retro_get_memory_data(RETRO_MEMORY_RTC), rtc_data.data(), sz);
    }

    retro_system_av_info av_info = {};
    core->retro_get_system_av_info(&av_info);
    base_width = av_info.geometry.base_width;
    base_height = av_info.geometry.base_height;
    max_width = av_info.geometry.max_width;
    max_height = av_info.geometry.max_height;
    aspect_ratio = av_info.geometry.aspect_ratio;
    fps = av_info.timing.fps;

    audio->start(g_cfg.get_mono_audio(), av_info.timing.sample_rate, g_cfg.get_sample_rate(), av_info.timing.fps);
    frame_throttle->reset(fps);
    core->retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    video->resolution_changed(base_width, base_height, pixel_format == RETRO_PIXEL_FORMAT_XRGB8888 ? 32 : 16);

    char library_message[256];
    snprintf(library_message, 256, "Loaded core: %s", library_name.c_str());
    video->set_message(library_message, lround(fps * 5));
}

}
