///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Anycubic_hpp_
#define slic3r_Anycubic_hpp_

#include <memory>
#include <string>
#include <vector>

#include <wx/string.h>

#include "PrintHost.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

class DynamicPrintConfig;

struct AnycubicAceSlot
{
    int box { 0 };
    int index { 0 };
    bool empty { true };
    bool has_color { false };
    int red { 0 };
    int green { 0 };
    int blue { 0 };
    std::string material;
};

struct AnycubicReadings
{
    bool has_nozzle { false };
    bool has_bed { false };
    bool has_chamber { false };
    bool has_model_fan { false };
    bool has_aux_fan { false };
    bool has_light { false };
    bool has_ace_count { false };
    bool ace_reported { false };
    int ace_count { 0 };
    std::vector<AnycubicAceSlot> ace_slots;
    int nozzle { 0 };
    int bed { 0 };
    int chamber { 0 };
    int model_fan { 0 };
    int aux_fan { 0 };
    bool light_on { false };
    // A print report. has_job means the printer answered, even when it is idle.
    bool has_job { false };
    bool printing { false };
    bool has_progress { false };
    bool has_layer { false };
    bool has_total_layers { false };
    bool has_remain { false };
    bool has_elapsed { false };
    std::string job_name;
    int progress { 0 };
    int layer { 0 };
    int layers { 0 };
    int remain_minutes { 0 };
    int elapsed_minutes { 0 };
};

// Stock Anycubic LAN mode, the same connection Anycubic Slicer Next uses.
// The printer must have Settings → Network → LAN Mode turned on. Nothing is installed on the printer.
class Anycubic : public PrintHost
{
public:
    explicit Anycubic(DynamicPrintConfig* config);
    ~Anycubic() override = default;

    const char* get_name() const override;

    bool test(wxString& curl_msg) const override;
    wxString get_test_ok_msg() const override;
    wxString get_test_failed_msg(wxString& msg) const override;
    bool upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    bool has_auto_discovery() const override { return false; }
    bool can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }
    std::string get_host() const override { return m_host; }

    // Chamber light and camera capture use the printer's LAN command channel.
    bool set_chamber_light(bool on, std::string& error) const;
    bool set_camera_capture(bool start, std::string& error) const;
    // box is 0 for the first ACE unit. index is the 0-based slot. empty clears that slot.
    bool set_ace_slot(int box, int index, int red, int green, int blue, const std::string& material, bool empty, std::string& error) const;
    // Asks the printer for the temperatures, fan speeds, and chamber light it can report.
    bool read_status(AnycubicReadings& readings, std::string& error) const;

private:
    std::string m_host;
};

// One decoded camera frame. rgb is width * height * 3 bytes, tightly packed.
struct AnycubicPicture
{
    int width { 0 };
    int height { 0 };
    int serial { 0 };
    std::vector<unsigned char> rgb;
};

// Reads the printer's camera stream and keeps the newest picture.
class AnycubicPreview
{
public:
    explicit AnycubicPreview(std::string host);
    ~AnycubicPreview();

    AnycubicPreview(const AnycubicPreview&) = delete;
    AnycubicPreview& operator=(const AnycubicPreview&) = delete;

    void start();
    void stop();
    bool copy_picture(AnycubicPicture& picture);
    std::string last_error() const;

private:
    struct State;
    std::shared_ptr<State> m_state;
};

}

#endif
