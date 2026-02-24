SET(SOURCES components/ui_comp_header.c
    components/ui_comp.c
    screens/ui_startScreen.c
    screens/ui_mainScreen.c
    screens/ui_audioScreen.c
    screens/ui_cameraScreen.c
    screens/ui_connectivityScreen.c
    screens/ui_mainCommsScreen.c
    screens/ui_canScreen.c
    screens/ui_buzzerScreen.c
    ui_theme_manager.c
    ui_themes.c
    ui.c
    components/ui_comp_hook.c
    ui_helpers.c
    ui_events.c
    images/ui_img_mil_lamp_png.c
    images/ui_img_high_beam_png.c
    images/ui_img_low_beam_png.c
    images/ui_img_267822735.c
    images/ui_img_1669329308.c
    images/ui_img_cruise_control_png.c
    images/ui_img_limite_50_png.c
    fonts/ui_font_energyBatteryFont.c
    fonts/ui_font_energyBatteryLabelFont.c
    fonts/ui_font_hmLabel.c
    fonts/ui_font_shiftLabel.c
    fonts/ui_font_speedFont.c
    fonts/ui_font_speedLabelFont.c
    fonts/ui_font_tripTotalLabel.c)

add_library(ui ${SOURCES})
