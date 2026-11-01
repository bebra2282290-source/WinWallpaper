#include "Strings.h"

#include <windows.h>

namespace {

struct Entry {
    const wchar_t* ru;
    const wchar_t* en;
};

// Order must match enum class S.
const Entry kStrings[] = {
    {L"WinWallpaper", L"WinWallpaper"},
    {L"Видео не выбрано", L"No video selected"},
    {L"воспроизводится", L"playing"},
    {L"пауза", L"paused"},
    {L"остановлено", L"stopped"},
    {L"включена вручную", L"by you"},
    {L"открыто полноэкранное приложение", L"a fullscreen app is running"},
    {L"рабочий стол закрыт окнами", L"the desktop is covered by windows"},
    {L"работа от батареи", L"running on battery"},
    {L"компьютер заблокирован", L"the computer is locked"},
    {L"экран выключен", L"the display is off"},
    {L"рабочий стол не найден", L"desktop not found"},
    {L"Выбрать видео…", L"Choose video…"},
    {L"Пауза", L"Pause"},
    {L"Масштаб", L"Scaling"},
    {L"Заполнить экран (с обрезкой)", L"Fill the screen (crop)"},
    {L"Вписать целиком (с полями)", L"Fit the whole video (bars)"},
    {L"Растянуть", L"Stretch"},
    {L"Звук", L"Sound"},
    {L"Выключен", L"Off"},
    {L"Частота кадров", L"Frame rate"},
    {L"Как в видео", L"Same as the video"},
    {L"Не больше 30 кадров/с", L"Up to 30 fps"},
    {L"Не больше 15 кадров/с (макс. экономия)", L"Up to 15 fps (max savings)"},
    {L"Экономия ресурсов", L"Resource saving"},
    {L"Пауза, когда открыта игра или полноэкранное приложение", L"Pause while a game or fullscreen app is running"},
    {L"Пауза, когда рабочий стол закрыт окнами", L"Pause while the desktop is covered by windows"},
    {L"Пауза при работе от батареи и в режиме экономии энергии", L"Pause on battery power and in energy saver mode"},
    {L"Запускать вместе с Windows", L"Start with Windows"},
    {L"Убрать видеообои", L"Remove the video wallpaper"},
    {L"Выход", L"Exit"},
    {L"Выберите видео для обоев", L"Choose a video for the wallpaper"},
    {L"Видео", L"Video files"},
    {L"Все файлы", L"All files"},
    {L"Нажмите на значок в трее, чтобы выбрать видео и настроить обои.",
     L"Click the tray icon to choose a video and adjust the wallpaper."},
    {L"Файл видео не найден:", L"Video file not found:"},
    {L"Не удалось воспроизвести видео. Возможно, формат не поддерживается — попробуйте MP4 (H.264).",
     L"Could not play the video. The format may be unsupported; try MP4 (H.264)."},
    {L"Не удалось встроиться в рабочий стол. Попробую ещё раз автоматически.",
     L"Could not attach to the desktop. Will retry automatically."},
    {L"Не удалось инициализировать Direct3D 11. Обновите драйвер видеокарты.",
     L"Could not initialize Direct3D 11. Please update the graphics driver."},
    {L"Media Foundation недоступен. В редакциях Windows N установите «Пакет компонентов мультимедиа» (Media Feature Pack).",
     L"Media Foundation is not available. On Windows N editions, install the Media Feature Pack."},
    {L"Код ошибки", L"Error code"},
};

static_assert(sizeof(kStrings) / sizeof(kStrings[0]) == static_cast<size_t>(S::Count),
              "kStrings must match enum class S");

bool UseRussian() {
    static const bool russian = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
    return russian;
}

} // namespace

const wchar_t* T(S id) {
    const Entry& entry = kStrings[static_cast<int>(id)];
    return UseRussian() ? entry.ru : entry.en;
}
