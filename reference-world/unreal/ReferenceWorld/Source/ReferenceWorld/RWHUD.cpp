#include "RWHUD.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/World.h"

void ARWHUD::DrawHUD()
{
	Super::DrawHUD();
	if (!Canvas)
	{
		return;
	}

	const float CX = Canvas->ClipX * 0.5f;
	const float CY = Canvas->ClipY * 0.5f;
	DrawRect(FLinearColor(1.f, 1.f, 1.f, 0.55f), CX - 2.f, CY - 2.f, 4.f, 4.f);

	const UWorld* World = GetWorld();
	const float T = World ? World->GetTimeSeconds() : 0.f;
	if (T < HintSeconds)
	{
		const float Alpha = FMath::Clamp((HintSeconds - T) / 2.f, 0.f, 1.f);
		UFont* Font = GEngine ? GEngine->GetMediumFont() : nullptr;
		const FString Hint = TEXT("WASD move    Mouse look    Space jump    Shift sprint");
		float W = 0.f;
		float H = 0.f;
		GetTextSize(Hint, W, H, Font, 1.f);
		DrawText(Hint, FLinearColor(1.f, 0.95f, 0.85f, Alpha), CX - W * 0.5f, Canvas->ClipY * 0.86f, Font, 1.f);
	}
}
