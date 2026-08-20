#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <QMainWindow>
#include <QPushButton>
#include <QBoxLayout>
#include <QPointer>
#include <QDialog>
#include <QRadioButton>
#include <QListWidget>
#include <QDialogButtonBox>
#include <QStringList>
#include <QLabel>
#include <QLineEdit>
#include <QTimer>

#include <functional>
#include <memory>
#include <cstring>
#include <algorithm>

#include <obs-frontend-api.h>

extern "C" {
#include "proxy-config.h"
#include "config.h"
}
#include "dock.h"

static QPointer<QPushButton> g_btn;
static QPointer<QPushButton> g_selbtn;

static void update_style(QPushButton *b)
{
    const bool on = g_cfg.enabled;
    b->setChecked(on);
    b->setText(on ? QStringLiteral("Proxy: ON") : QStringLiteral("Enable Proxy"));
    b->setStyleSheet(on
        ? QStringLiteral("QPushButton{background:#2d7d46;color:#fff;font-weight:bold;padding:6px;}")
        : QStringLiteral("QPushButton{padding:6px;}"));
}

static QString mode_label()
{
    if (g_cfg.select_mode == PROXY_SELECT_RANDOM)
        return QStringLiteral("Proxy: Random \u25BE");
    if (g_cfg.select_mode == PROXY_SELECT_MANUAL) {
        QString s = QStringLiteral("Proxy: (manual) \u25BE");
        EnterCriticalSection(&g_cfg.lock);
        int idx = g_cfg.selected_index;
        if (idx >= 0 && idx < g_cfg.list_count) {
            s = QStringLiteral("Proxy: %1:%2 \u25BE")
                    .arg(QString::fromUtf8(g_cfg.list[idx].host))
                    .arg(g_cfg.list[idx].port);
        }
        LeaveCriticalSection(&g_cfg.lock);
        return s;
    }
    return QStringLiteral("Proxy: Round-robin \u25BE");
}

static void set_list_url(const QString &url)
{
    QByteArray ba = url.trimmed().toUtf8();
    int n = std::min<int>((int)sizeof(g_cfg.list_url) - 1, ba.size());
    memcpy(g_cfg.list_url, ba.constData(), (size_t)n);
    g_cfg.list_url[n] = '\0';
}

static void open_select_dialog(QWidget *parent)
{
    int curIdx, mode;
    EnterCriticalSection(&g_cfg.lock);
    curIdx = g_cfg.selected_index;
    mode = g_cfg.select_mode;
    LeaveCriticalSection(&g_cfg.lock);

    QDialog dlg(parent);
    dlg.setWindowTitle(QStringLiteral("Proxy List"));
    dlg.resize(420, 470);
    QVBoxLayout *v = new QVBoxLayout(&dlg);

    // --- List URL row (editable) + Reload ---
    v->addWidget(new QLabel(QStringLiteral("Proxy list URL:"), &dlg));
    QHBoxLayout *urlRow = new QHBoxLayout();
    QLineEdit *urlEdit = new QLineEdit(QString::fromUtf8(g_cfg.list_url), &dlg);
    urlEdit->setPlaceholderText(QStringLiteral("https://example.com/proxy.txt"));
    QPushButton *reloadBtn = new QPushButton(QStringLiteral("Reload"), &dlg);
    urlRow->addWidget(urlEdit, 1);
    urlRow->addWidget(reloadBtn);
    v->addLayout(urlRow);

    QLabel *status = new QLabel(QString(), &dlg);
    v->addWidget(status);

    QLabel *info = new QLabel(&dlg);
    QRadioButton *rrb = new QRadioButton(
        QStringLiteral("Round-robin (rotate each connection)"), &dlg);
    QRadioButton *rnd = new QRadioButton(QStringLiteral("Random"), &dlg);
    QRadioButton *spec = new QRadioButton(QStringLiteral("Specific proxy:"), &dlg);
    QListWidget *lw = new QListWidget(&dlg);

    QDialogButtonBox *bb = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);

    v->addWidget(info);
    v->addWidget(rrb);
    v->addWidget(rnd);
    v->addWidget(spec);
    v->addWidget(lw, 1);
    v->addWidget(bb);

    // Fill the list widget from the current fetched proxies.
    std::function<int()> populate = [lw, info]() -> int {
        lw->clear();
        EnterCriticalSection(&g_cfg.lock);
        int c = g_cfg.list_count;
        for (int i = 0; i < c; i++) {
            const struct proxy_entry &e = g_cfg.list[i];
            QString t = QStringLiteral("%1:%2")
                            .arg(QString::fromUtf8(e.host)).arg(e.port);
            if (e.use_auth)     t += QStringLiteral("  (auth)");
            if (!e.resolved_ok) t += QStringLiteral("  [unresolved]");
            lw->addItem(t);
        }
        LeaveCriticalSection(&g_cfg.lock);
        info->setText(QStringLiteral("%1 proxies in list").arg(c));
        return c;
    };
    populate();

    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    QObject::connect(spec, &QRadioButton::toggled, lw, &QListWidget::setEnabled);
    QObject::connect(lw, &QListWidget::itemDoubleClicked, &dlg, &QDialog::accept);

    // Reload: save the URL, restart the background fetch, then poll to refresh.
    QObject::connect(reloadBtn, &QPushButton::clicked, &dlg, [=, &dlg]() {
        set_list_url(urlEdit->text());
        g_cfg.use_list = true;
        obsproxy_save_config();
        obsproxy_apply_runtime();

        if (g_cfg.list_url[0] == '\0') {
            status->setText(QStringLiteral("Enter a URL first."));
            return;
        }
        status->setText(QStringLiteral("Reloading\u2026"));

        auto tries = std::make_shared<int>(0);
        QTimer *t = new QTimer(&dlg);
        t->setInterval(500);
        QObject::connect(t, &QTimer::timeout, &dlg, [=]() {
            int c = populate();
            (*tries)++;
            if (c > 0 || *tries >= 20) {
                status->setText(c > 0
                    ? QStringLiteral("Loaded %1 proxies").arg(c)
                    : QStringLiteral("No proxies loaded (check URL)"));
                t->stop();
                t->deleteLater();
            }
        });
        t->start();
    });

    if (mode == PROXY_SELECT_RANDOM) {
        rnd->setChecked(true);
    } else if (mode == PROXY_SELECT_MANUAL) {
        spec->setChecked(true);
        if (curIdx >= 0 && curIdx < lw->count())
            lw->setCurrentRow(curIdx);
    } else {
        rrb->setChecked(true);
    }
    lw->setEnabled(spec->isChecked());

    if (dlg.exec() == QDialog::Accepted) {
        set_list_url(urlEdit->text());
        if (rnd->isChecked()) {
            g_cfg.select_mode = PROXY_SELECT_RANDOM;
        } else if (spec->isChecked()) {
            g_cfg.select_mode = PROXY_SELECT_MANUAL;
            g_cfg.selected_index = lw->currentRow();
        } else {
            g_cfg.select_mode = PROXY_SELECT_ROUNDROBIN;
        }
        obsproxy_save_config();
        obsproxy_apply_runtime();
        if (g_selbtn)
            g_selbtn->setText(mode_label());
    }
}

static void inject_button()
{
    if (g_btn)
        return;

    QMainWindow *mw = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    if (!mw)
        return;

    // Buttons in the Controls dock live in this vertical layout (see
    // frontend/forms/OBSBasicControls.ui).
    QVBoxLayout *lay = mw->findChild<QVBoxLayout *>(QStringLiteral("buttonsVLayout"));
    if (!lay)
        return;

    QPushButton *b = new QPushButton();
    b->setObjectName(QStringLiteral("obsProxyEnableButton"));
    b->setCheckable(true);
    update_style(b);

    QObject::connect(b, &QPushButton::clicked, [b](bool checked) {
        g_cfg.enabled = checked;
        obsproxy_save_config();
        obsproxy_apply_runtime();
        update_style(b);
    });

    lay->addWidget(b);
    g_btn = b;

    // "Select Proxy" button -> popup to pick round-robin / random / a specific proxy.
    QPushButton *sb = new QPushButton();
    sb->setObjectName(QStringLiteral("obsProxySelectButton"));
    sb->setText(mode_label());
    sb->setToolTip(QStringLiteral(
        "Change the proxy list URL, reload the list, and pick "
        "round-robin / random / a specific proxy."));
    sb->setStyleSheet(QStringLiteral("QPushButton{padding:6px;}"));
    QObject::connect(sb, &QPushButton::clicked, [mw](bool) {
        open_select_dialog(mw);
    });
    lay->addWidget(sb);
    g_selbtn = sb;
}

static void on_frontend_event(enum obs_frontend_event event, void *)
{
    if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
        inject_button();
}

extern "C" void proxy_dock_register(void)
{
    obs_frontend_add_event_callback(on_frontend_event, nullptr);
}

extern "C" void proxy_dock_unregister(void)
{
    obs_frontend_remove_event_callback(on_frontend_event, nullptr);
    if (g_btn) {
        g_btn->deleteLater();
        g_btn = nullptr;
    }
    if (g_selbtn) {
        g_selbtn->deleteLater();
        g_selbtn = nullptr;
    }
}
