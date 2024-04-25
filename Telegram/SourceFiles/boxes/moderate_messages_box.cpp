/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/moderate_messages_box.h"

#include "boxes/delete_messages_box.h"
#include "boxes/peers/edit_peer_permissions_box.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_peer.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "ui/controls/userpic_button.h"
#include "ui/effects/ripple_animation.h"
#include "ui/effects/toggle_arrow.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rect.h"
#include "ui/rect_part.h"
#include "ui/text/text_utilities.h"
#include "ui/vertical_list.h"
#include "ui/widgets/checkbox.h"
#include "ui/wrap/slide_wrap.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"

namespace {

enum class ModerateOption {
	Ban = (1 << 0),
	DeleteAll = (1 << 1),
};
inline constexpr bool is_flag_type(ModerateOption) { return true; }
using ModerateOptions = base::flags<ModerateOption>;

ModerateOptions CalculateModerateOptions(const HistoryItemsList &items) {
	Expects(!items.empty());

	const auto peer = items.front()->history()->peer;
	auto allCanBan = true;
	auto allCanDelete = true;
	for (const auto &item : items) {
		if (!allCanBan && !allCanDelete) {
			return ModerateOptions(0);
		}
		if (peer != item->history()->peer) {
			return ModerateOptions(0);
		}
		if (!item->suggestBanReport()) {
			allCanBan = false;
		}
		if (!item->suggestDeleteAllReport()) {
			allCanDelete = false;
		}
	}
	return ModerateOptions(0)
		| (allCanBan ? ModerateOption::Ban : ModerateOptions(0))
		| (allCanDelete ? ModerateOption::DeleteAll : ModerateOptions(0));
}

class Button final : public Ui::RippleButton {
public:
	Button(not_null<QWidget*> parent, int count);

	void setChecked(bool checked);
	[[nodiscard]] bool checked() const;

	[[nodiscard]] static QSize ComputeSize(int);

private:
	void paintEvent(QPaintEvent *event) override;
	QImage prepareRippleMask() const override;
	QPoint prepareRippleStartPosition() const override;

	const int _count;
	const QString _text;
	bool _checked = false;

	Ui::Animations::Simple _animation;

};

Button::Button(not_null<QWidget*> parent, int count)
: RippleButton(parent, st::defaultRippleAnimation)
, _count(count)
, _text(QString::number(std::abs(_count))) {
}

QSize Button::ComputeSize(int count) {
	return QSize(
		st::moderateBoxExpandHeight
			+ st::moderateBoxExpand.width()
			+ st::moderateBoxExpandInnerSkip * 4
			+ st::moderateBoxExpandFont->width(
				QString::number(std::abs(count)))
			+ st::moderateBoxExpandToggleSize,
		st::moderateBoxExpandHeight);
}

void Button::setChecked(bool checked) {
	if (_checked == checked) {
		return;
	}
	_checked = checked;
	_animation.stop();
	_animation.start(
		[=] { update(); },
		checked ? 0 : 1,
		checked ? 1 : 0,
		st::slideWrapDuration);
}

bool Button::checked() const {
	return _checked;
}

void Button::paintEvent(QPaintEvent *event) {
	auto p = Painter(this);
	auto hq = PainterHighQualityEnabler(p);
	Ui::RippleButton::paintRipple(p, QPoint());
	const auto radius = height() / 2;
	p.setPen(Qt::NoPen);
	st::moderateBoxExpand.paint(
		p,
		radius,
		(height() - st::moderateBoxExpand.height()) / 2,
		width());

	const auto innerSkip = st::moderateBoxExpandInnerSkip;

	p.setBrush(Qt::NoBrush);
	p.setPen(st::boxTextFg);
	p.setFont(st::moderateBoxExpandFont);
	p.drawText(
		QRect(
			innerSkip + radius + st::moderateBoxExpand.width(),
			0,
			width(),
			height()),
		_text,
		style::al_left);

	const auto path = Ui::ToggleUpDownArrowPath(
		width() - st::moderateBoxExpandToggleSize - radius,
		height() / 2,
		st::moderateBoxExpandToggleSize,
		st::moderateBoxExpandToggleFourStrokes,
		_animation.value(_checked ? 1. : 0.));
	p.fillPath(path, st::boxTextFg);
}

QImage Button::prepareRippleMask() const {
	return Ui::RippleAnimation::RoundRectMask(size(), size().height() / 2);
}

QPoint Button::prepareRippleStartPosition() const {
	return mapFromGlobal(QCursor::pos());
}

} // namespace

void CreateModerateMessagesBox(
		not_null<Ui::GenericBox*> box,
		const HistoryItemsList &items) {
	const auto options = CalculateModerateOptions(items);
	const auto inner = box->verticalLayout();

	const auto users = [&] {
		auto result = std::vector<not_null<UserData*>>();
		for (const auto &item : items) {
			if (const auto user = item->from()->asUser()) {
				if (!ranges::contains(result, not_null{ user })) {
					result.push_back(user);
				}
			}
		}
		return result;
	}();
	Assert(!users.empty());

	const auto isSingle = users.size() == 1;
	const auto buttonPadding = isSingle
		? QMargins()
		: QMargins(0, 0, Button::ComputeSize(users.size()).width(), 0);

	struct Controller final {
		rpl::event_stream<bool> toggleRequestsFromTop;
		rpl::event_stream<bool> toggleRequestsFromInner;
		rpl::event_stream<bool> checkAllRequests;
	};
	const auto createUsersList = [&](not_null<Controller*> controller) {
		const auto wrap = inner->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				inner,
				object_ptr<Ui::VerticalLayout>(inner)));
		wrap->toggle(false, anim::type::instant);

		controller->toggleRequestsFromTop.events(
		) | rpl::start_with_next([=](bool toggled) {
			wrap->toggle(toggled, anim::type::normal);
		}, wrap->lifetime());

		const auto container = wrap->entity();
		Ui::AddSkip(container);

		auto &lifetime = wrap->lifetime();
		const auto clicks = lifetime.make_state<rpl::event_stream<>>();
		const auto checkboxes = ranges::views::all(
			users
		) | ranges::views::transform([&](not_null<UserData*> user) {
			const auto line = container->add(
				object_ptr<Ui::AbstractButton>(container));
			const auto &st = st::moderateBoxUserpic;
			line->resize(line->width(), st.size.height());

			const auto userpic = Ui::CreateChild<Ui::UserpicButton>(
				line,
				user,
				st);
			const auto checkbox = Ui::CreateChild<Ui::Checkbox>(
				line,
				user->name(),
				false,
				st::defaultBoxCheckbox);
			line->widthValue(
			) | rpl::start_with_next([=](int width) {
				userpic->moveToLeft(
					st::boxRowPadding.left()
						+ checkbox->checkRect().width()
						+ st::defaultBoxCheckbox.textPosition.x(),
					0);
				const auto skip = st::defaultBoxCheckbox.textPosition.x();
				checkbox->resizeToWidth(width
					- rect::right(userpic)
					- skip
					- st::boxRowPadding.right());
				checkbox->moveToLeft(
					rect::right(userpic) + skip,
					((userpic->height() - checkbox->height()) / 2)
						+ st::defaultBoxCheckbox.margin.top());
			}, checkbox->lifetime());

			userpic->setAttribute(Qt::WA_TransparentForMouseEvents);
			checkbox->setAttribute(Qt::WA_TransparentForMouseEvents);

			line->setClickedCallback([=] {
				checkbox->setChecked(!checkbox->checked());
				clicks->fire({});
			});

			return checkbox;
		}) | ranges::to_vector;

		clicks->events(
		) | rpl::start_with_next([=] {
			controller->toggleRequestsFromInner.fire_copy(
				ranges::any_of(checkboxes, &Ui::Checkbox::checked));
		}, container->lifetime());

		controller->checkAllRequests.events(
		) | rpl::start_with_next([=](bool checked) {
			for (const auto &c : checkboxes) {
				c->setChecked(checked);
			}
		}, container->lifetime());
	};

	const auto appendList = [&](not_null<Ui::Checkbox*> checkbox) {
		const auto button = Ui::CreateChild<Button>(inner, users.size());
		button->resize(Button::ComputeSize(users.size()));

		const auto overlay = Ui::CreateChild<Ui::AbstractButton>(inner);

		checkbox->geometryValue(
		) | rpl::start_with_next([=](const QRect &rect) {
			overlay->setGeometry(rect);
			overlay->raise();

			button->moveToRight(
				st::moderateBoxExpandRight,
				rect.top() + (rect.height() - button->height()) / 2,
				box->width());
			button->raise();
		}, button->lifetime());

		const auto controller = checkbox->lifetime().make_state<Controller>();
		controller->toggleRequestsFromInner.events(
		) | rpl::start_with_next([=](bool toggled) {
			checkbox->setChecked(toggled);
		}, checkbox->lifetime());
		button->setClickedCallback([=] {
			button->setChecked(!button->checked());
			controller->toggleRequestsFromTop.fire_copy(button->checked());
		});
		overlay->setClickedCallback([=] {
			checkbox->setChecked(!checkbox->checked());
			controller->checkAllRequests.fire_copy(checkbox->checked());
		});
		createUsersList(controller);
	};

	Ui::AddSkip(inner);
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			(items.size() == 1)
				? tr::lng_selected_delete_sure_this()
				: tr::lng_selected_delete_sure(
					lt_count,
					rpl::single(items.size()) | tr::to_count()),
			st::boxLabel));
	Ui::AddSkip(inner);
	Ui::AddSkip(inner);
	Ui::AddSkip(inner);
	{
		const auto report = box->addRow(
			object_ptr<Ui::Checkbox>(
				box,
				tr::lng_report_spam(tr::now),
				false,
				st::defaultBoxCheckbox),
			st::boxRowPadding + buttonPadding);
		if (!isSingle) {
			appendList(report);
		}
	}

	if (options & ModerateOption::DeleteAll) {
		Ui::AddSkip(inner);
		Ui::AddSkip(inner);

		const auto deleteAll = inner->add(
			object_ptr<Ui::Checkbox>(
				inner,
				!(isSingle)
					? tr::lng_delete_all_from_users(
						tr::now,
						Ui::Text::WithEntities)
					: tr::lng_delete_all_from_user(
						tr::now,
						lt_user,
						Ui::Text::Bold(items.front()->from()->name()),
						Ui::Text::WithEntities),
				false,
				st::defaultBoxCheckbox),
			st::boxRowPadding + buttonPadding);
		if (!isSingle) {
			appendList(deleteAll);
		}
	}
	if (options & ModerateOption::Ban) {
		auto ownedWrap = object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			inner,
			object_ptr<Ui::VerticalLayout>(inner));

		Ui::AddSkip(inner);
		Ui::AddSkip(inner);
		const auto ban = inner->add(
			object_ptr<Ui::Checkbox>(
				box,
				rpl::conditional(
					ownedWrap->toggledValue(),
					tr::lng_context_restrict_user(),
					rpl::conditional(
						rpl::single(isSingle),
						tr::lng_ban_user(),
						tr::lng_ban_users())),
				false,
				st::defaultBoxCheckbox),
			st::boxRowPadding + buttonPadding);
		if (!isSingle) {
			appendList(ban);
		}
		Ui::AddSkip(inner);
		Ui::AddSkip(inner);

		const auto wrap = inner->add(std::move(ownedWrap));
		const auto container = wrap->entity();
		wrap->toggle(false, anim::type::instant);
		auto label = object_ptr<Ui::FlatLabel>(
			inner,
			wrap->toggledValue(
			) | rpl::map([isSingle](bool toggled) {
				return Ui::Text::Link(
					((toggled && isSingle)
						? tr::lng_restrict_user_part
						: (toggled && !isSingle)
						? tr::lng_restrict_users_part
						: isSingle
						? tr::lng_restrict_user_full
						: tr::lng_restrict_users_full)(tr::now),
					u"internal:"_q);
			}),
			st::boxDividerLabel);

		auto &lifetime = wrap->lifetime();
		const auto scrollLifetime = lifetime.make_state<rpl::lifetime>();
		label->setClickHandlerFilter([=](
				const ClickHandlerPtr &handler,
				Qt::MouseButton button) {
			if (button != Qt::LeftButton) {
				return false;
			}
			wrap->toggle(!wrap->toggled(), anim::type::normal);
			{
				const auto start = crl::now();
				inner->heightValue(
				) | rpl::start_with_next([=] {
					if (!wrap->animating()) {
						scrollLifetime->destroy();
						Ui::PostponeCall(crl::guard(box, [=] {
							box->scrollToY(std::numeric_limits<int>::max());
						}));
					} else {
						box->scrollToY(std::numeric_limits<int>::max());
					}
				}, *scrollLifetime);
			}
			return true;
		});

		Ui::AddSkip(inner);
		inner->add(object_ptr<Ui::DividerLabel>(
			inner,
			std::move(label),
			st::defaultBoxDividerLabelPadding,
			RectPart::Top | RectPart::Bottom));

		using Flag = ChatRestriction;
		using Flags = ChatRestrictions;
		const auto peer = items.front()->history()->peer;
		const auto chat = peer->asChat();
		const auto channel = peer->asChannel();
		const auto defaultRestrictions = chat
			? chat->defaultRestrictions()
			: channel->defaultRestrictions();
		const auto prepareFlags = FixDependentRestrictions(
			defaultRestrictions
			| ((channel && channel->isPublic())
				? (Flag::ChangeInfo | Flag::PinMessages)
				: Flags(0)));
		const auto disabledMessages = [&] {
			auto result = base::flat_map<Flags, QString>();
			{
				const auto disabled = FixDependentRestrictions(
					defaultRestrictions
					| ((channel && channel->isPublic())
						? (Flag::ChangeInfo | Flag::PinMessages)
						: Flags(0)));
				result.emplace(
					disabled,
					tr::lng_rights_restriction_for_all(tr::now));
			}
			return result;
		}();

		auto [checkboxes, getRestrictions, changes] = CreateEditRestrictions(
			box,
			rpl::conditional(
				rpl::single(isSingle),
				tr::lng_restrict_users_part_single_header(),
				tr::lng_restrict_users_part_header(
					lt_count,
					rpl::single(users.size()) | tr::to_count())),
			prepareFlags,
			disabledMessages,
			{ .isForum = peer->isForum() });
		std::move(
			changes
		) | rpl::start_with_next([=] {
			ban->setChecked(true);
		}, ban->lifetime());
		Ui::AddSkip(container);
		Ui::AddDivider(container);
		Ui::AddSkip(container);
		container->add(std::move(checkboxes));
	}
	Ui::AddSkip(inner);

	box->addButton(tr::lng_box_delete(), [=] { box->closeBox(); });
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

bool CanCreateModerateMessagesBox(const HistoryItemsList &items) {
	const auto options = CalculateModerateOptions(items);
	return (options & ModerateOption::Ban)
		|| (options & ModerateOption::DeleteAll);
}
