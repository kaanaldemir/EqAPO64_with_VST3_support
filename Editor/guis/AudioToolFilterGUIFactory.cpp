#include <QGridLayout>
#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "filters/VUMeterProtocol.h"
#include "AudioToolFilterGUIFactory.h"

using namespace std;

static QString tokenValue(const QString& parameters, const QString& key, const QString& defaultValue)
{
	QStringList parts = parameters.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
	for (int i = 0; i + 1 < parts.size(); ++i)
		if (parts[i].compare(key, Qt::CaseInsensitive) == 0)
			return parts[i + 1];
	return defaultValue;
}

static double tokenDouble(const QString& parameters, const QString& key, double defaultValue)
{
	bool ok = false;
	double value = tokenValue(parameters, key, QString()).toDouble(&ok);
	return ok ? value : defaultValue;
}

static double dbFromLinear(double value)
{
	return value > 0.000000000001 ? 20.0 * log10(value) : -90.0;
}

class VUMeterPanel : public QWidget
{
public:
	explicit VUMeterPanel(QWidget* parent = nullptr)
		: QWidget(parent)
	{
		setMinimumSize(560, 420);
		timer.setInterval(33);
		connect(&timer, &QTimer::timeout, this, [this]() {
			readSharedData();
			update();
		});
		timer.start();
	}

	~VUMeterPanel() override
	{
		disconnectMeter();
	}

	void setMeterId(const QString& value)
	{
		QString normalized = value.trimmed().isEmpty() ? "default" : value.trimmed();
		if (meterId == normalized)
			return;
		meterId = normalized;
		disconnectMeter();
	}

	void reset()
	{
		if (connectMeter())
			shared->resetRequest++;
	}

	bool hasValidData() const { return valid; }

protected:
	void paintEvent(QPaintEvent*) override
	{
		QPainter p(this);
		p.fillRect(rect(), QColor(13, 15, 18));
		p.setRenderHint(QPainter::Antialiasing, true);

		const QRect inner = rect().adjusted(16, 14, -16, -14);
		p.setPen(QColor(225, 230, 235));
		QFont titleFont = p.font();
		titleFont.setBold(true);
		titleFont.setPointSize(titleFont.pointSize() + 1);
		p.setFont(titleFont);
		p.drawText(inner.left(), inner.top() + 16, tr("APO Loudness / VU Meter"));
		QFont normalFont = p.font();
		normalFont.setBold(false);
		normalFont.setPointSize(normalFont.pointSize() - 1);
		p.setFont(normalFont);
		p.setPen(QColor(150, 158, 168));
		p.drawText(inner.left(), inner.top() + 36, tr("MeterId %1 - ITU-R BS.1770 / EBU R128 style readout").arg(meterId));

		if (!valid)
		{
			p.setPen(QColor(180, 185, 190));
			p.drawText(inner.adjusted(0, 64, 0, 0), tr("Waiting for VUMeter: MeterId %1").arg(meterId));
			return;
		}

		const int top = inner.top() + 72;
		const int bottom = inner.bottom() - 84;
		const int meterHeight = bottom - top;
		const int channelCount = max(1, static_cast<int>((std::min)(data.channelCount, static_cast<std::uint32_t>(VUMETER_MAX_CHANNELS))));
		const int leftScale = 48;
		const int rightPanel = 188;
		const int available = max(80, inner.width() - leftScale - rightPanel);
		const int barWidth = max(8, available / max(1, channelCount * 3));
		const int pairGap = max(4, barWidth / 3);
		const int channelGap = max(8, barWidth);

		p.setPen(QColor(85, 90, 98));
		for (int db = 6; db >= -60; db -= 6)
		{
			const int y = bottom - static_cast<int>((db + 60) / 66.0 * meterHeight);
			p.drawLine(inner.left() + leftScale - 6, y, inner.left() + leftScale + available - 4, y);
			if (db % 12 == 0 || db == 6)
			{
				p.setPen(QColor(160, 166, 174));
				p.drawText(QRect(inner.left(), y - 8, leftScale - 10, 16), Qt::AlignRight | Qt::AlignVCenter, QString::number(db));
				p.setPen(QColor(85, 90, 98));
			}
		}

		int x = inner.left() + leftScale;
		double maxPeak = 0.0;
		double maxRms = 0.0;
		qulonglong clips = 0;
		for (int ch = 0; ch < channelCount; ++ch)
		{
			maxPeak = max(maxPeak, data.peak[ch]);
			maxRms = max(maxRms, data.rms[ch]);
			clips += data.clip[ch];
			drawBar(p, QRect(x, top, barWidth, meterHeight), dbFromLinear(data.rms[ch]), ch, false);
			drawBar(p, QRect(x + barWidth + pairGap, top, barWidth, meterHeight), dbFromLinear(data.peak[ch]), ch, true);
			p.setPen(QColor(210, 215, 220));
			p.drawText(QRect(x - 4, bottom + 6, barWidth * 2 + pairGap + 8, 16), Qt::AlignCenter, channelLabel(ch));
			p.setPen(QColor(120, 128, 136));
			p.drawText(QRect(x - 4, bottom + 22, barWidth * 2 + pairGap + 8, 16), Qt::AlignCenter, "RMS  PK");
			x += 2 * barWidth + pairGap + channelGap;
		}

		const QRect panel(inner.right() - rightPanel + 8, top, rightPanel - 8, meterHeight + 42);
		p.setPen(QColor(60, 64, 70));
		p.setBrush(QColor(22, 25, 29));
		p.drawRoundedRect(panel, 4, 4);
		drawMetric(p, panel, 14, "Momentary", QString("%1 LUFS").arg(data.lufsMomentary, 0, 'f', 1), QColor(84, 205, 255));
		drawMetric(p, panel, 58, "Short-term", QString("%1 LUFS").arg(data.lufsShortTerm, 0, 'f', 1), QColor(111, 232, 124));
		drawMetric(p, panel, 102, "Integrated", QString("%1 LUFS").arg(data.lufsIntegrated, 0, 'f', 1), QColor(250, 220, 80));
		drawMetric(p, panel, 146, "True peak*", QString("%1 dBFS").arg(dbFromLinear(maxPeak), 0, 'f', 1), QColor(255, 128, 96));
		drawMetric(p, panel, 190, "Max RMS", QString("%1 dBFS").arg(dbFromLinear(maxRms), 0, 'f', 1), QColor(180, 190, 205));
		drawMetric(p, panel, 234, "Clips", QString::number(clips), clips ? QColor(255, 80, 80) : QColor(180, 190, 205));

		p.setPen(QColor(125, 132, 142));
		p.drawText(QRect(inner.left(), inner.bottom() - 32, inner.width(), 30), Qt::AlignLeft | Qt::AlignVCenter,
			tr("Scale: dBFS. LUFS windows: M 400 ms, S 3 s, I session-gated approximation. *Sample peak in APO path."));
	}

private:
	QString channelLabel(int ch) const
	{
		static const char* labels[] = {"L", "R", "C", "LFE", "RL", "RR", "SL", "SR"};
		if (ch >= 0 && ch < 8)
			return labels[ch];
		return QString::number(ch + 1);
	}

	void drawMetric(QPainter& p, const QRect& panel, int y, const QString& name, const QString& value, const QColor& color)
	{
		const QRect row(panel.left() + 12, panel.top() + y, panel.width() - 24, 34);
		p.setPen(QColor(145, 152, 162));
		p.drawText(row, Qt::AlignLeft | Qt::AlignTop, name);
		QFont f = p.font();
		f.setBold(true);
		f.setPointSize(f.pointSize() + 2);
		p.setFont(f);
		p.setPen(color);
		p.drawText(row, Qt::AlignLeft | Qt::AlignBottom, value);
		f.setBold(false);
		f.setPointSize(f.pointSize() - 2);
		p.setFont(f);
	}

	void drawBar(QPainter& p, const QRect& bar, double db, int channel, bool peak)
	{
		p.setPen(QColor(65, 70, 76));
		p.setBrush(QColor(28, 31, 36));
		p.drawRoundedRect(bar, 2, 2);

		const double clamped = (std::max)(-60.0, (std::min)(6.0, db));
		const int fillHeight = static_cast<int>((clamped + 60.0) / 66.0 * bar.height());
		QRect fill = bar.adjusted(2, bar.height() - fillHeight + 1, -2, -2);
		QLinearGradient gradient(fill.topLeft(), fill.bottomLeft());
		gradient.setColorAt(0.0, QColor(250, 60, 60));
		gradient.setColorAt(0.18, QColor(245, 220, 55));
		gradient.setColorAt(0.38, QColor(90, 230, 90));
		gradient.setColorAt(1.0, QColor(70, 210, 190));
		p.fillRect(fill, gradient);

		if (peak)
		{
			const int y = bar.bottom() - static_cast<int>(((std::max)(-60.0, (std::min)(6.0, dbFromLinear(data.peakHold[channel]))) + 60.0) / 66.0 * bar.height());
			p.setPen(QColor(255, 245, 90));
			p.drawLine(bar.left() + 2, y, bar.right() - 2, y);
		}

		p.setPen(QColor(215, 220, 226));
		p.drawText(QRect(bar.left() - 10, bar.top() - 18, bar.width() + 20, 14), Qt::AlignCenter, QString("%1").arg(db, 0, 'f', 1));
	}

	bool connectMeter()
	{
		if (shared != nullptr)
			return true;
		const QString objectName = QStringLiteral("Global\\EqAPO_VUMeter_") + meterId;
		mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, reinterpret_cast<const wchar_t*>(objectName.utf16()));
		if (mapping == NULL)
			return false;
		shared = static_cast<VUMeterSharedData*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(VUMeterSharedData)));
		if (shared == nullptr)
		{
			CloseHandle(mapping);
			mapping = NULL;
			return false;
		}
		return true;
	}

	void disconnectMeter()
	{
		if (shared != nullptr)
		{
			UnmapViewOfFile(shared);
			shared = nullptr;
		}
		if (mapping != NULL)
		{
			CloseHandle(mapping);
			mapping = NULL;
		}
		valid = false;
	}

	void readSharedData()
	{
		valid = false;
		if (!connectMeter())
			return;
		if (shared->magic != VUMETER_MAGIC || shared->version != VUMETER_VERSION)
		{
			disconnectMeter();
			return;
		}
		memcpy(&data, shared, sizeof(data));
		valid = true;
	}

	QString meterId = "default";
	QTimer timer;
	HANDLE mapping = NULL;
	VUMeterSharedData* shared = nullptr;
	VUMeterSharedData data = {};
	bool valid = false;
};

QWidget* AudioToolFilterGUI::addSliderControl(QGridLayout* grid, const QString& label, QDoubleSpinBox** spin, double min, double max, double value, const QString& suffix, int row, int column, int decimals)
{
	QWidget* box = new QWidget(this);
	QVBoxLayout* layout = new QVBoxLayout(box);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(2);
	QLabel* title = new QLabel(label, box);
	QSlider* slider = new QSlider(Qt::Horizontal, box);
	*spin = new QDoubleSpinBox(box);
	(*spin)->setRange(min, max);
	(*spin)->setDecimals(decimals);
	(*spin)->setSuffix(suffix);
	(*spin)->setValue(value);
	(*spin)->setKeyboardTracking(false);
	slider->setRange(0, 1000);
	slider->setValue(static_cast<int>((value - min) / (max - min) * 1000.0));
	layout->addWidget(title);
	layout->addWidget(slider);
	layout->addWidget(*spin);
	connect(slider, &QSlider::valueChanged, this, [this, spin, min, max](int sliderValue) {
		const double value = min + (max - min) * sliderValue / 1000.0;
		(*spin)->setValue(value);
	});
	connect(*spin, static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged), this, [this, slider, min, max](double value) {
		QSignalBlocker blocker(slider);
		slider->setValue(static_cast<int>((value - min) / (max - min) * 1000.0));
		emit updateModel();
	});
	grid->addWidget(box, row, column);
	return box;
}

void AudioToolFilterGUI::addChannelSelector(QGridLayout* grid, const QString& parameters, int row, int column, int columnSpan)
{
	QWidget* box = new QWidget(this);
	QHBoxLayout* layout = new QHBoxLayout(box);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(4);
	layout->addWidget(new QLabel(tr("Channels"), box));
	const QStringList names = QStringList() << "L" << "R" << "C" << "LFE" << "RL" << "RR" << "SL" << "SR";
	const QString channels = tokenValue(parameters, "Channels", "all").toUpper();
	const QStringList listedChannels = channels.split(',', Qt::SkipEmptyParts);
	for (const QString& name : names)
	{
		QCheckBox* check = new QCheckBox(name, box);
		check->setChecked(channels == "ALL"
			|| (channels == "STEREO" && (name == "L" || name == "R"))
			|| (channels == "MONO" && name == "L")
			|| listedChannels.contains(name));
		channelChecks.append(check);
		layout->addWidget(check);
		connect(check, &QCheckBox::toggled, this, [this](bool) { emit updateModel(); });
	}
	layout->addStretch(1);
	grid->addWidget(box, row, column, 1, columnSpan);
}

QString AudioToolFilterGUI::selectedChannels() const
{
	QStringList selected;
	for (QCheckBox* check : channelChecks)
		if (check->isChecked())
			selected << check->text();
	if (selected.size() == channelChecks.size())
		return "all";
	const QStringList mono = QStringList() << "L";
	const QStringList stereo = QStringList() << "L" << "R";
	if (selected == mono)
		return "mono";
	if (selected == stereo)
		return "stereo";
	return selected.isEmpty() ? "all" : selected.join(",");
}

AudioToolFilterGUI::AudioToolFilterGUI(const QString& command, const QString& parameters)
	: commandName(command)
{
	QGridLayout* grid = new QGridLayout(this);
	grid->setContentsMargins(0, 0, 0, 0);
	grid->setHorizontalSpacing(8);
	grid->setVerticalSpacing(6);

	if (commandName == "ToneGenerator")
	{
		stateButton = new QPushButton(tokenDouble(parameters, "State", 0) != 0 ? tr("Stop") : tr("Play"), this);
		stateButton->setCheckable(true);
		stateButton->setChecked(tokenDouble(parameters, "State", 0) != 0);
		typeComboBox = new QComboBox(this);
		typeComboBox->addItems(QStringList() << "Sine" << "White" << "Pink" << "Brown" << "Sweep");
		typeComboBox->setCurrentText(tokenValue(parameters, "Type", "Sine"));
		modeComboBox = new QComboBox(this);
		modeComboBox->addItems(QStringList() << "Replace" << "Mix");
		modeComboBox->setCurrentText(tokenValue(parameters, "Mode", "Replace"));
		grid->addWidget(stateButton, 0, 0);
		grid->addWidget(typeComboBox, 0, 1);
		grid->addWidget(modeComboBox, 0, 2);
		addSliderControl(grid, tr("Frequency"), &frequencySpinBox, 20, 20000, tokenDouble(parameters, "Frequency", 1000), " Hz", 1, 0, 1);
		addSliderControl(grid, tr("Level"), &levelSpinBox, -80, 0, tokenDouble(parameters, "Level", -20), " dB", 1, 1, 1);
		addSliderControl(grid, tr("Sweep start"), &startSpinBox, 20, 20000, tokenDouble(parameters, "Start", 20), " Hz", 1, 2, 1);
		addSliderControl(grid, tr("Sweep end"), &endSpinBox, 20, 20000, tokenDouble(parameters, "End", 20000), " Hz", 1, 3, 1);
		addSliderControl(grid, tr("Duration"), &durationSpinBox, 1, 120, tokenDouble(parameters, "Duration", 10), " s", 1, 4, 1);
		addChannelSelector(grid, parameters, 2, 0, 5);
		connect(stateButton, &QPushButton::toggled, this, [this](bool checked) {
			stateButton->setText(checked ? tr("Stop") : tr("Play"));
			emit updateModel();
		});
	}
	else if (commandName == "Pan")
	{
		addSliderControl(grid, tr("Position"), &positionSpinBox, -100, 100, tokenDouble(parameters, "Position", 0), QString(), 0, 0, 1);
		addSliderControl(grid, tr("Width"), &widthSpinBox, 0, 200, tokenDouble(parameters, "Width", 100), " %", 0, 1, 1);
	}
	else if (commandName == "Chorus")
	{
		addSliderControl(grid, tr("Rate"), &rateSpinBox, 0.05, 5, tokenDouble(parameters, "Rate", 0.4), " Hz", 0, 0, 2);
		addSliderControl(grid, tr("Depth"), &depthSpinBox, 0, 30, tokenDouble(parameters, "Depth", 8), " ms", 0, 1, 1);
		addSliderControl(grid, tr("Mix"), &mixSpinBox, 0, 100, tokenDouble(parameters, "Mix", 25), " %", 0, 2, 1);
		addSliderControl(grid, tr("Feedback"), &feedbackSpinBox, -80, 80, tokenDouble(parameters, "Feedback", 0), " %", 0, 3, 1);
	}
	else if (commandName == "Reverb")
	{
		addSliderControl(grid, tr("Room"), &roomSpinBox, 0, 100, tokenDouble(parameters, "RoomSize", 50), " %", 0, 0, 1);
		addSliderControl(grid, tr("Damping"), &dampingSpinBox, 0, 100, tokenDouble(parameters, "Damping", 50), " %", 0, 1, 1);
		addSliderControl(grid, tr("Wet"), &wetSpinBox, 0, 100, tokenDouble(parameters, "Wet", 20), " %", 0, 2, 1);
		addSliderControl(grid, tr("Dry"), &drySpinBox, 0, 150, tokenDouble(parameters, "Dry", 100), " %", 0, 3, 1);
		addSliderControl(grid, tr("Width"), &widthSpinBox, 0, 100, tokenDouble(parameters, "Width", 100), " %", 0, 4, 1);
	}
	else if (commandName == "VUMeter")
	{
		panelButton = new QPushButton(tr("Open panel"), this);
		panelButton->setCheckable(true);
		QPushButton* resetButton = new QPushButton(tr("Reset"), this);
		grid->addWidget(panelButton, 0, 0);
		grid->addWidget(resetButton, 0, 1);
		addChannelSelector(grid, parameters, 1, 0, 4);
		meterDialog = new QDialog(nullptr, Qt::Window | Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);
		meterDialog->setWindowTitle(tr("APO Loudness / VU Meter"));
		meterDialog->setAttribute(Qt::WA_DeleteOnClose, false);
		meterDialog->resize(760, 520);
		QVBoxLayout* panelLayout = new QVBoxLayout(meterDialog);
		meterPanel = new VUMeterPanel(meterDialog);
		panelLayout->addWidget(meterPanel);
		updateMeterPanel();
		connect(panelButton, &QPushButton::toggled, this, [this](bool checked) {
			panelButton->setText(checked ? tr("Hide panel") : tr("Open panel"));
			if (checked)
			{
				meterDialog->setWindowState(meterDialog->windowState() & ~Qt::WindowMinimized);
				meterDialog->show();
				meterDialog->raise();
				meterDialog->activateWindow();
			}
			else
				meterDialog->hide();
		});
		connect(meterDialog, &QDialog::finished, this, [this](int) {
			if (panelButton != nullptr)
			{
				QSignalBlocker blocker(panelButton);
				panelButton->setChecked(false);
				panelButton->setText(tr("Open panel"));
			}
		});
		connect(resetButton, &QPushButton::clicked, this, [this]() { if (meterPanel) meterPanel->reset(); });
	}

	for (QComboBox* combo : findChildren<QComboBox*>())
		connect(combo, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this, [this](int) { emit updateModel(); });
}

AudioToolFilterGUI::~AudioToolFilterGUI()
{
	destroyMeterDialog();
}

void AudioToolFilterGUI::prepareDelete()
{
	destroyMeterDialog();
}

void AudioToolFilterGUI::destroyMeterDialog()
{
	if (meterDialog == nullptr)
		return;

	meterDialog->hide();
	delete meterDialog;
	meterDialog = nullptr;
	meterPanel = nullptr;
}

void AudioToolFilterGUI::updateMeterPanel()
{
	if (meterPanel != nullptr)
		meterPanel->setMeterId("default");
}

void AudioToolFilterGUI::store(QString& command, QString& parameters)
{
	command = commandName;
	if (commandName == "ToneGenerator")
	{
		parameters = QString("State %1 Type %2 Frequency %3 Hz Level %4 dB Channels %5 Mode %6")
			.arg(stateButton->isChecked() ? 1 : 0)
			.arg(typeComboBox->currentText())
			.arg(frequencySpinBox->value())
			.arg(levelSpinBox->value())
			.arg(selectedChannels())
			.arg(modeComboBox->currentText());
		if (typeComboBox->currentText() == "Sweep")
			parameters += QString(" Start %1 Hz End %2 Hz Duration %3 s").arg(startSpinBox->value()).arg(endSpinBox->value()).arg(durationSpinBox->value());
	}
	else if (commandName == "Pan")
		parameters = QString("Position %1 Width %2").arg(positionSpinBox->value()).arg(widthSpinBox->value());
	else if (commandName == "Chorus")
		parameters = QString("Rate %1 Hz Depth %2 ms Mix %3 % Feedback %4 %").arg(rateSpinBox->value()).arg(depthSpinBox->value()).arg(mixSpinBox->value()).arg(feedbackSpinBox->value());
	else if (commandName == "Reverb")
		parameters = QString("RoomSize %1 % Damping %2 % Wet %3 % Dry %4 % Width %5 %").arg(roomSpinBox->value()).arg(dampingSpinBox->value()).arg(wetSpinBox->value()).arg(drySpinBox->value()).arg(widthSpinBox->value());
	else if (commandName == "VUMeter")
		parameters = QString("MeterId default Channels %1").arg(selectedChannels());
}

QList<FilterTemplate> ToneGeneratorFilterGUIFactory::createFilterTemplates()
{
	return QList<FilterTemplate>() << FilterTemplate(tr("Tone generator"), "ToneGenerator: State 0 Type Sine Frequency 1000 Hz Level -20 dB Channels all Mode Replace", QStringList(tr("Basic filters")));
}

IFilterGUI* ToneGeneratorFilterGUIFactory::createFilterGUI(QString& command, QString& parameters)
{
	return command == "ToneGenerator" ? new AudioToolFilterGUI(command, parameters) : nullptr;
}

QList<FilterTemplate> PanFilterGUIFactory::createFilterTemplates()
{
	return QList<FilterTemplate>() << FilterTemplate(tr("Pan"), "Pan: Position 0 Width 100", QStringList(tr("Basic filters")));
}

IFilterGUI* PanFilterGUIFactory::createFilterGUI(QString& command, QString& parameters)
{
	return command == "Pan" ? new AudioToolFilterGUI(command, parameters) : nullptr;
}

QList<FilterTemplate> ChorusFilterGUIFactory::createFilterTemplates()
{
	return QList<FilterTemplate>() << FilterTemplate(tr("Chorus"), "Chorus: Rate 0.4 Hz Depth 8 ms Mix 25 % Feedback 0 %", QStringList(tr("Effects")));
}

IFilterGUI* ChorusFilterGUIFactory::createFilterGUI(QString& command, QString& parameters)
{
	return command == "Chorus" ? new AudioToolFilterGUI(command, parameters) : nullptr;
}

QList<FilterTemplate> ReverbFilterGUIFactory::createFilterTemplates()
{
	return QList<FilterTemplate>() << FilterTemplate(tr("Reverb"), "Reverb: RoomSize 50 % Damping 50 % Wet 20 % Dry 100 % Width 100 %", QStringList(tr("Effects")));
}

IFilterGUI* ReverbFilterGUIFactory::createFilterGUI(QString& command, QString& parameters)
{
	return command == "Reverb" ? new AudioToolFilterGUI(command, parameters) : nullptr;
}

QList<FilterTemplate> VUMeterFilterGUIFactory::createFilterTemplates()
{
	return QList<FilterTemplate>() << FilterTemplate(tr("VU meter"), "VUMeter: MeterId default Channels all", QStringList(tr("Analysis")));
}

IFilterGUI* VUMeterFilterGUIFactory::createFilterGUI(QString& command, QString& parameters)
{
	return command == "VUMeter" ? new AudioToolFilterGUI(command, parameters) : nullptr;
}
