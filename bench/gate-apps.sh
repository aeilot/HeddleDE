# 门槛实验的应用命令（容器内 root 运行；chromium 因此需要 --no-sandbox）。
CHROMIUM='chromium --no-sandbox --ozone-platform=wayland --disable-gpu --no-first-run --no-default-browser-check --password-store=basic --user-data-dir=/tmp/chromium-profile file:///src/bench/pages/form.html'
TEXTEDIT='dbus-run-session -- gnome-text-editor --standalone'
WRITER='SAL_USE_VCLPLUGIN=gtk3 dbus-run-session -- soffice --writer --norestore --nologo'
