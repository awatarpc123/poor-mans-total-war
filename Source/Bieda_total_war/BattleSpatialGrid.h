#pragma once

#include "CoreMinimal.h"

/**
 * Lightweight 2D spatial hash grid for fast radius-based neighbour queries.
 *
 * Replaces O(n^2) brute-force pair checks with O(n*k) where k = local
 * density (average agents per cell neighbourhood).  The caller's position
 * array is kept by reference — it must outlive the grid.
 *
 * Usage:
 *   TArray<FVector> Positions;          // filled during snapshot pass
 *   FBattleSpatialGrid Grid;
 *   Grid.Build(Positions, 500.f);       // cell size ~ most-common query radius
 *   Grid.ForEachInRadius(Center, 300.f, SelfIdx,
 *       [&](int32 j, float DistSq2D) { ... });
 */
class FBattleSpatialGrid
{
public:
	/**
	 * Insert all positions into the hash grid.
	 * @param InPositions  Position array — must outlive the grid (no deep copy).
	 * @param InCellSize   Grid cell side in cm.  Best perf when ~= your most-common query radius.
	 */
	void Build(const TArray<FVector>& InPositions, float InCellSize = 500.f)
	{
		Pos     = &InPositions;
		Cell    = InCellSize;
		InvCell = 1.f / Cell;
		Buckets.Reset();

		for (int32 i = 0; i < InPositions.Num(); ++i)
			Buckets.FindOrAdd(Hash(InPositions[i])).Add(i);
	}

	/**
	 * Invoke Fn(int32 Index, float DistSquared2D) for every point
	 * within Radius of Center (2D distance), skipping SelfIdx.
	 */
	template<typename TFunc>
	void ForEachInRadius(const FVector& Center, float Radius,
	                     int32 SelfIdx, TFunc&& Fn) const
	{
		const float RSq = Radius * Radius;
		const int32 LoX = FMath::FloorToInt((Center.X - Radius) * InvCell);
		const int32 HiX = FMath::FloorToInt((Center.X + Radius) * InvCell);
		const int32 LoY = FMath::FloorToInt((Center.Y - Radius) * InvCell);
		const int32 HiY = FMath::FloorToInt((Center.Y + Radius) * InvCell);

		for (int32 cx = LoX; cx <= HiX; ++cx)
		{
			for (int32 cy = LoY; cy <= HiY; ++cy)
			{
				const TArray<int32>* Bucket = Buckets.Find(BKey(cx, cy));
				if (!Bucket) continue;

				for (int32 j : *Bucket)
				{
					if (j == SelfIdx) continue;
					const float D2 = ((*Pos)[j] - Center).SizeSquared2D();
					if (D2 < RSq)
						Fn(j, D2);
				}
			}
		}
	}

private:
	const TArray<FVector>*     Pos     = nullptr;
	float                      Cell    = 500.f;
	float                      InvCell = 1.f / 500.f;
	TMap<int64, TArray<int32>> Buckets;

	int64 Hash(const FVector& P) const
	{
		return BKey(FMath::FloorToInt(P.X * InvCell),
		            FMath::FloorToInt(P.Y * InvCell));
	}

	static int64 BKey(int32 X, int32 Y)
	{
		return (static_cast<int64>(X) << 32) | static_cast<uint32>(Y);
	}
};
