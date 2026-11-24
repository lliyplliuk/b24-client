#pragma once

#include <QString>
#include <algorithm>

namespace bx::ui {

struct ReactionChoice {
    const char *code;
    const char *emoji;
    const char *caption;
};

// Portal ReactionName keys with native emoji and labels for keyboard and screen readers.
inline constexpr ReactionChoice reactionChoices[] = {
    {"like", "👍", "Нравится"},
    {"faceWithTearsOfJoy", "😂", "До слёз"},
    {"redHeart", "❤️", "Сердце"},
    {"neutralFace", "😐", "Без эмоций"},
    {"fire", "🔥", "Огонь"},
    {"cry", "😢", "Грустно"},
    {"slightlySmilingFace", "🙂", "Улыбка"},
    {"handshake", "🤝", "Рукопожатие"},
    {"hundredPoints", "💯", "Сто баллов"},
    {"sleepingSymbol", "💤", "Сон"},
    {"crossMark", "❌", "Красный крестик"},
    {"whiteHeavyCheckMark", "✅", "Готово"},
    {"eyes", "👀", "Смотрю"},
    {"winkingFace", "😉", "Подмигнуть"},
    {"laugh", "😄", "Смех"},
    {"kiss", "😘", "Поцелуй"},
    {"wonder", "😮", "Удивление"},
    {"slightlyFrowningFace", "🙁", "Недовольство"},
    {"loudlyCryingFace", "😭", "Рыдаю"},
    {"faceWithStuckOutTongue", "😛", "Дразнюсь"},
    {"faceWithStuckOutTongueAndWinkingEye", "😜", "Шучу"},
    {"smilingFaceWithSunglasses", "😎", "Круто"},
    {"confusedFace", "😕", "В замешательстве"},
    {"flushedFace", "😳", "Смущение"},
    {"thinkingFace", "🤔", "Думаю"},
    {"angry", "😠", "Злость"},
    {"smilingFaceWithHorns", "😈", "Чёртик"},
    {"faceWithThermometer", "🤒", "Болею"},
    {"facepalm", "🤦", "Рука у лица"},
    {"poo", "💩", "Какашка"},
    {"flexedBiceps", "💪", "Сила"},
    {"clappingHands", "👏", "Аплодисменты"},
    {"raisedHand", "✋", "Поднимаю руку"},
    {"dislike", "👎", "Не нравится"},
    {"smilingFaceWithHeartEyes", "😍", "Восхищение"},
    {"smilingFaceWithHearts", "🥰", "Любовь"},
    {"pleadingFace", "🥺", "Пожалуйста"},
    {"relievedFace", "😌", "Облегчение"},
    {"foldedHands", "🙏", "Спасибо"},
    {"okHand", "👌", "Всё в порядке"},
    {"signHorns", "🤘", "Рок"},
    {"loveYouGesture", "🤟", "Люблю"},
    {"clownFace", "🤡", "Клоун"},
    {"partyingFace", "🥳", "Праздник"},
    {"questionMark", "❓", "Вопрос"},
    {"exclamationMark", "❗", "Важно"},
    {"lightBulb", "💡", "Идея"},
    {"bomb", "💣", "Бомба"},
    {"saluteFace", "🫡", "Салют"},
    {"sunny", "☀️", "Солнце"},
    {"newMoonFace", "🌚", "Луна"},
    {"brokenHeart", "💔", "Разбитое сердце"},
    {"nailPolish", "💅", "Маникюр"},
    {"meltingFace", "🫠", "Таю"},
    {"plusOne", "➕", "Плюс один"},
    {"smilingFaceWithTear", "🥲", "Улыбка со слезой"},
    {"wiltedFlower", "🥀", "Увядший цветок"},
    {"smilingFaceWithHalo", "😇", "Ангел"},
};

inline const ReactionChoice *findReaction(QStringView code)
{
    const auto item = std::find_if(std::begin(reactionChoices), std::end(reactionChoices),
                                   [code](const ReactionChoice &reaction) { return code == QLatin1String(reaction.code); });
    return item == std::end(reactionChoices) ? nullptr : &*item;
}

} // namespace bx::ui
