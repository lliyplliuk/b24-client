#pragma once

#include <QPainter>
#include <QPainterPath>

namespace bx::ui {

enum class Glyph {
    Chat, Projects, Notices, Clock, Search, Refresh, Close, Attach, Emoji, Send, Check, Unread, Copy, Link, More,
    Task, Person, File, Archive, Android, Pdf, Document, Spreadsheet, Presentation, Image, Audio, Video, Code, Executable
};

inline void paintGlyph(QPainter &p, Glyph glyph, const QRectF &box, const QColor &color)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(box.topLeft());
    p.scale(box.width() / 24, box.height() / 24);
    p.setPen(QPen(color, 1.65, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    QPainterPath path;
    switch (glyph) {
    case Glyph::Chat:
        path.moveTo(5, 5); path.lineTo(19, 5); path.quadTo(21, 5, 21, 8);
        path.lineTo(21, 14); path.quadTo(21, 17, 18, 17); path.lineTo(10, 17);
        path.lineTo(5, 21); path.lineTo(5, 17); path.quadTo(3, 17, 3, 14);
        path.lineTo(3, 8); path.quadTo(3, 5, 5, 5);
        p.drawPath(path);
        p.drawLine(QPointF(8, 10), QPointF(16, 10));
        p.drawLine(QPointF(8, 13), QPointF(13, 13));
        break;
    case Glyph::Projects:
    case Glyph::Archive:
        path.moveTo(3, 7); path.quadTo(3, 5, 5, 5); path.lineTo(10, 5); path.lineTo(12, 8);
        path.lineTo(19, 8); path.quadTo(21, 8, 21, 10); path.lineTo(21, 17);
        path.quadTo(21, 19, 19, 19); path.lineTo(5, 19); path.quadTo(3, 19, 3, 17);
        path.closeSubpath();
        p.drawPath(path);
        if (glyph == Glyph::Archive) {
            p.drawLine(QPointF(12.5, 11), QPointF(14, 11));
            p.drawLine(QPointF(14, 13.6), QPointF(15.5, 13.6));
            p.drawLine(QPointF(12.5, 16.2), QPointF(14, 16.2));
        } else {
            p.drawLine(QPointF(8, 13), QPointF(16, 13));
        }
        break;
    case Glyph::Notices:
        path.moveTo(5, 16); path.quadTo(7, 15, 7, 11); path.quadTo(7, 6, 12, 6);
        path.quadTo(17, 6, 17, 11); path.quadTo(17, 15, 19, 16);
        path.lineTo(19, 17); path.lineTo(5, 17); path.closeSubpath();
        p.drawPath(path);
        p.drawLine(QPointF(12, 4), QPointF(12, 6));
        p.drawArc(QRectF(9, 14, 6, 6), 200 * 16, 140 * 16);
        break;
    case Glyph::Clock:
        p.drawEllipse(QRectF(4, 4, 16, 16));
        path.moveTo(12, 7); path.lineTo(12, 12); path.lineTo(15, 14);
        p.drawPath(path);
        break;
    case Glyph::Search:
        p.drawEllipse(QRectF(4, 4, 12, 12));
        p.drawLine(QPointF(14.5, 14.5), QPointF(20, 20));
        break;
    case Glyph::Refresh:
        p.drawArc(QRectF(4, 4, 16, 16), 40 * 16, 295 * 16);
        path.moveTo(20, 4); path.lineTo(20, 9); path.lineTo(15, 9);
        p.drawPath(path);
        break;
    case Glyph::Close:
        p.drawLine(QPointF(7, 7), QPointF(17, 17));
        p.drawLine(QPointF(17, 7), QPointF(7, 17));
        break;
    case Glyph::Attach:
        path.moveTo(15, 8); path.lineTo(9, 14); path.quadTo(7, 16, 9, 18);
        path.quadTo(11, 20, 13, 18); path.lineTo(20, 11); path.quadTo(23, 8, 20, 5);
        path.quadTo(17, 2, 14, 5); path.lineTo(5, 14); path.quadTo(2, 17, 5, 20);
        path.quadTo(8, 23, 11, 20);
        p.drawPath(path);
        break;
    case Glyph::Emoji:
        p.drawEllipse(QRectF(3.5, 3.5, 17, 17));
        p.drawPoint(QPointF(9, 10));
        p.drawPoint(QPointF(15, 10));
        path.moveTo(8, 13.5); path.quadTo(12, 17.5, 16, 13.5);
        p.drawPath(path);
        break;
    case Glyph::Send:
        path.moveTo(4, 11); path.lineTo(20, 5); path.lineTo(15, 20); path.lineTo(11, 14);
        path.closeSubpath();
        p.drawPath(path);
        p.drawLine(QPointF(11, 14), QPointF(20, 5));
        break;
    case Glyph::Check:
        path.moveTo(5, 12); path.lineTo(10, 17); path.lineTo(19, 7);
        p.drawPath(path);
        break;
    case Glyph::Unread:
        p.drawEllipse(QRectF(4, 4, 16, 16));
        p.drawPoint(QPointF(12, 12));
        break;
    case Glyph::Copy:
        p.drawRoundedRect(QRectF(9, 9, 12, 12), 2, 2);
        path.moveTo(6, 15); path.lineTo(5, 15); path.quadTo(3, 15, 3, 13);
        path.lineTo(3, 5); path.quadTo(3, 3, 5, 3); path.lineTo(13, 3);
        path.quadTo(15, 3, 15, 5); path.lineTo(15, 6);
        p.drawPath(path);
        break;
    case Glyph::Link:
        path.moveTo(10, 8); path.lineTo(14, 4); path.cubicTo(18, 0, 24, 6, 20, 10);
        path.lineTo(16, 14);
        path.moveTo(8, 10); path.lineTo(4, 14); path.cubicTo(0, 18, 6, 24, 10, 20);
        path.lineTo(14, 16);
        p.drawPath(path);
        p.drawLine(QPointF(9, 15), QPointF(15, 9));
        break;
    case Glyph::Task:
        p.drawRoundedRect(QRectF(5, 3, 14, 18), 2, 2);
        path.moveTo(8, 12); path.lineTo(11, 15); path.lineTo(16, 9);
        p.drawPath(path);
        break;
    case Glyph::Person:
        p.drawEllipse(QRectF(8, 4, 8, 8));
        path.moveTo(5, 20); path.lineTo(5, 18);
        path.cubicTo(5, 14, 19, 14, 19, 18); path.lineTo(19, 20);
        p.drawPath(path);
        break;
    case Glyph::File:
    case Glyph::Document:
    case Glyph::Pdf:
        path.moveTo(5, 4); path.quadTo(5, 3, 7, 3); path.lineTo(14, 3); path.lineTo(19, 8);
        path.lineTo(19, 19); path.quadTo(19, 21, 17, 21); path.lineTo(7, 21);
        path.quadTo(5, 21, 5, 19); path.closeSubpath();
        path.moveTo(14, 3); path.lineTo(14, 8); path.lineTo(19, 8);
        p.drawPath(path);
        if (glyph == Glyph::Document) {
            p.drawLine(QPointF(8.5, 12), QPointF(15.5, 12));
            p.drawLine(QPointF(8.5, 16), QPointF(15.5, 16));
        } else if (glyph == Glyph::Pdf) {
            QPainterPath ribbon(QPointF(8.5, 17));
            ribbon.cubicTo(10.5, 16, 12.5, 12.5, 12.5, 10.5);
            ribbon.cubicTo(11, 12, 12.8, 16.4, 15.4, 16.8);
            ribbon.cubicTo(16.5, 15.3, 12, 15, 8.5, 17);
            p.drawPath(ribbon);
        }
        break;
    case Glyph::Android:
        path.moveTo(3, 17.5);
        path.cubicTo(3.5, 12, 7.4, 8.5, 12, 8.5);
        path.cubicTo(16.6, 8.5, 20.5, 12, 21, 17.5);
        path.closeSubpath();
        p.drawPath(path);
        p.drawLine(QPointF(7.8, 9.5), QPointF(5.8, 6));
        p.drawLine(QPointF(16.2, 9.5), QPointF(18.2, 6));
        p.setPen(QPen(color, 2.4, Qt::SolidLine, Qt::RoundCap));
        p.drawPoint(QPointF(8.5, 14));
        p.drawPoint(QPointF(15.5, 14));
        break;
    case Glyph::Spreadsheet:
        p.drawRoundedRect(QRectF(4, 4, 16, 16), 1.6, 1.6);
        p.drawLine(QPointF(4.5, 9.5), QPointF(19.5, 9.5));
        p.drawLine(QPointF(4.5, 14.5), QPointF(19.5, 14.5));
        p.drawLine(QPointF(10, 9.5), QPointF(10, 19.5));
        break;
    case Glyph::Presentation:
        p.drawRoundedRect(QRectF(3, 4.5, 18, 12), 1.5, 1.5);
        p.drawEllipse(QRectF(9.2, 7.9, 5.6, 5.6));
        p.drawLine(QPointF(12, 16.5), QPointF(12, 20));
        p.drawLine(QPointF(8.5, 20), QPointF(15.5, 20));
        break;
    case Glyph::Image:
        p.drawRoundedRect(QRectF(3, 4.5, 18, 15), 1.5, 1.5);
        p.drawEllipse(QRectF(7, 8, 2, 2));
        path.moveTo(4, 17); path.lineTo(8.5, 13); path.lineTo(11, 15);
        path.lineTo(15, 11); path.lineTo(20.5, 16.5);
        p.drawPath(path);
        break;
    case Glyph::Audio:
        path.moveTo(10.5, 16.5); path.lineTo(10.5, 6); path.lineTo(18.5, 4);
        path.lineTo(18.5, 14);
        p.drawPath(path);
        p.drawEllipse(QRectF(5, 15.5, 5.5, 3.5));
        p.drawEllipse(QRectF(13, 13, 5.5, 3.5));
        break;
    case Glyph::Video:
        p.drawRoundedRect(QRectF(3, 5, 18, 14), 2, 2);
        path.moveTo(10, 8.5); path.lineTo(15.5, 12); path.lineTo(10, 15.5);
        path.closeSubpath();
        p.drawPath(path);
        break;
    case Glyph::Code:
        path.moveTo(7.5, 8); path.lineTo(3.5, 12); path.lineTo(7.5, 16);
        path.moveTo(16.5, 8); path.lineTo(20.5, 12); path.lineTo(16.5, 16);
        p.drawPath(path);
        p.drawLine(QPointF(13.5, 6.5), QPointF(10.5, 17.5));
        break;
    case Glyph::Executable:
        p.drawRoundedRect(QRectF(3, 5, 18, 14), 2, 2);
        path.moveTo(7.5, 9.5); path.lineTo(10.5, 12); path.lineTo(7.5, 14.5);
        p.drawPath(path);
        p.drawLine(QPointF(12.5, 14.5), QPointF(16.5, 14.5));
        break;
    case Glyph::More: {
        p.setPen(QPen(color, 3.1, Qt::SolidLine, Qt::RoundCap));
        const QPointF dots[]{QPointF(5, 12), QPointF(12, 12), QPointF(19, 12)};
        p.drawPoints(dots, 3);
        break;
    }
    }
    p.restore();
}

} // namespace bx::ui
