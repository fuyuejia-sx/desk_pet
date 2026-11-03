#pragma once
#include <windows.h>
#include "pet.h"
#include "bubble.h"

// Open the chat window for this pet. Its bubble shows a short line of the reply.
void chat_open(HWND owner, HINSTANCE inst, Pet *pet, TextBubble *bubble);
// The pet window is going away; drop any pointer to its bubble.
void chat_forget(TextBubble *bubble);
