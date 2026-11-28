#pragma once
// NVN window crops are four signed 32-bit integers: x, y, width, height.
struct PresentationArea { int x, y, width, height; };
inline PresentationArea ResolvePresentationArea(int textureWidth, int textureHeight, PresentationArea crop)
{
    if (textureWidth <= 0 || textureHeight <= 0)
        return {0, 0, 0, 0};
    if (crop.x < 0 || crop.y < 0 || crop.width <= 0 || crop.height <= 0 ||
        crop.x >= textureWidth || crop.y >= textureHeight)
        return {0, 0, textureWidth, textureHeight};
    if (crop.width > textureWidth - crop.x) crop.width = textureWidth - crop.x;
    if (crop.height > textureHeight - crop.y) crop.height = textureHeight - crop.y;
    return crop;
}
