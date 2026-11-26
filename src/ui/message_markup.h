#pragma once

#include "core/message_markup.h"

namespace bx::workspace {

using markup::AttributedMessageQuote;
using markup::MessageMarkup;
using markup::leadingAttributedMessageQuote;
using markup::messageLinkTarget;
using markup::messageWebLink;
using markup::parseMessageMarkup;

inline QString markupPlainText(const QString &source)
{
    return markup::plainText(source);
}

} // namespace bx::workspace
