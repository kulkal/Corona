#include "stdafx.h"
#include "Corona.AssetExplorer.h"

#include "Corona.h"
#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#include "external/streamline-sdk/external/json/include/nlohmann/json.hpp"

#include "PlatformSystem.h"
#include "Utils.h"

void AppendCpuRuntimeTrace(const std::wstring& line);

namespace
{
	constexpr float kPanelWidth = 320.0f;
	constexpr float kToggleBtnH = 28.0f;
	constexpr float kBottomBarH = 38.0f; // shared with scene inspector

	bool EndsWithCi(const std::string& s, const std::string& suffix)
	{
		if (s.size() < suffix.size())
			return false;
		for (size_t i = 0; i < suffix.size(); ++i)
		{
			if (std::tolower(static_cast<unsigned char>(s[s.size() - suffix.size() + i])) !=
				std::tolower(static_cast<unsigned char>(suffix[i])))
				return false;
		}
		return true;
	}

	bool IsModelFile(const std::filesystem::path& p)
	{
		const std::string ext = p.extension().string();
		return EndsWithCi(ext, ".obj") || EndsWithCi(ext, ".fbx") || EndsWithCi(ext, ".cmesh");
	}

	bool IsMapFile(const std::filesystem::path& p)
	{
		const std::string ext = p.extension().string();
		return EndsWithCi(ext, ".map");
	}

	bool IsWorldLayoutFile(const std::filesystem::path& p)
	{
		const std::string ext = p.extension().string();
		return EndsWithCi(ext, ".worldlayout") || EndsWithCi(ext, ".wlayout");
	}

	bool IsRoadGraphFile(const std::filesystem::path& p)
	{
		const std::string ext = p.extension().string();
		return EndsWithCi(ext, ".roadgraph") || EndsWithCi(ext, ".rgraph");
	}

	bool IsCityLayoutFile(const std::filesystem::path& p)
	{
		const std::string ext = p.extension().string();
		return EndsWithCi(ext, ".citylayout") || EndsWithCi(ext, ".city.json");
	}

	bool IsImageFile(const std::filesystem::path& p)
	{
		const std::string ext = p.extension().string();
		return EndsWithCi(ext, ".png") || EndsWithCi(ext, ".jpg") ||
		       EndsWithCi(ext, ".jpeg") || EndsWithCi(ext, ".bmp");
	}

	bool IsMeshCacheFile(const std::filesystem::path& p)
	{
		return EndsWithCi(p.extension().string(), ".cmesh");
	}

	// Anything we can hand to LoadSceneForScript and spawn: source models plus
	// the baked .cmesh cache (LoadModel resolves a .cmesh path to itself).
	bool IsSpawnableMesh(const std::filesystem::path& p)
	{
		return IsModelFile(p) || IsMeshCacheFile(p);
	}

	// For a source model path "foo.fbx", return "foo.cmesh" (sibling).
	std::filesystem::path MeshCachePathFor(const std::filesystem::path& sourcePath)
	{
		std::filesystem::path cachePath = sourcePath;
		cachePath.replace_extension(L".cmesh");
		return cachePath;
	}

	std::string Trim(std::string value)
	{
		size_t first = 0;
		while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])))
			++first;
		size_t last = value.size();
		while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])))
			--last;
		return value.substr(first, last - first);
	}

	std::string StripComment(const std::string& line)
	{
		bool inQuote = false;
		for (size_t i = 0; i < line.size(); ++i)
		{
			const char c = line[i];
			if (c == '"' && (i == 0 || line[i - 1] != '\\'))
				inQuote = !inQuote;
			else if (c == '#' && !inQuote)
				return line.substr(0, i);
		}
		return line;
	}

	std::vector<std::string> TokenizeLayoutLine(const std::string& line)
	{
		std::vector<std::string> out;
		std::string token;
		bool inQuote = false;
		for (size_t i = 0; i < line.size(); ++i)
		{
			const char c = line[i];
			if (c == '"' && (i == 0 || line[i - 1] != '\\'))
			{
				inQuote = !inQuote;
				token.push_back(c);
				continue;
			}
			if (std::isspace(static_cast<unsigned char>(c)) && !inQuote)
			{
				if (!token.empty())
				{
					out.push_back(token);
					token.clear();
				}
				continue;
			}
			token.push_back(c);
		}
		if (!token.empty())
			out.push_back(token);
		return out;
	}

	std::string Unquote(std::string value)
	{
		value = Trim(value);
		if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
		{
			std::string out;
			out.reserve(value.size() - 2);
			for (size_t i = 1; i + 1 < value.size(); ++i)
			{
				if (value[i] == '\\' && i + 2 < value.size())
				{
					++i;
					out.push_back(value[i]);
				}
				else
				{
					out.push_back(value[i]);
				}
			}
			return out;
		}
		return value;
	}

	std::unordered_map<std::string, std::string> ParseKeyValues(const std::vector<std::string>& tokens, size_t first)
	{
		std::unordered_map<std::string, std::string> values;
		for (size_t i = first; i < tokens.size(); ++i)
		{
			const size_t eq = tokens[i].find('=');
			if (eq == std::string::npos || eq == 0)
				continue;
			std::string key = tokens[i].substr(0, eq);
			std::string value = tokens[i].substr(eq + 1);
			std::transform(key.begin(), key.end(), key.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			values[key] = Unquote(value);
		}
		return values;
	}

	bool ParseFloat(const std::string& value, float& out)
	{
		try
		{
			size_t consumed = 0;
			out = std::stof(value, &consumed);
			return consumed > 0;
		}
		catch (...)
		{
			return false;
		}
	}

	float ParseFloatOr(const std::unordered_map<std::string, std::string>& values, const char* key, float fallback)
	{
		auto it = values.find(key);
		if (it == values.end())
			return fallback;
		float parsed = fallback;
		return ParseFloat(it->second, parsed) ? parsed : fallback;
	}

	int ParseIntOr(const std::unordered_map<std::string, std::string>& values, const char* key, int fallback)
	{
		auto it = values.find(key);
		if (it == values.end())
			return fallback;
		try
		{
			return std::stoi(it->second);
		}
		catch (...)
		{
			return fallback;
		}
	}

	bool ParseFloatPair(std::string value, float& x, float& y)
	{
		value = Trim(value);
		if (!value.empty() && (value.front() == '(' || value.front() == '[' || value.front() == '{'))
			value.erase(value.begin());
		if (!value.empty() && (value.back() == ')' || value.back() == ']' || value.back() == '}'))
			value.pop_back();
		const size_t comma = value.find(',');
		if (comma == std::string::npos)
			return false;
		float parsedX = 0.0f;
		float parsedY = 0.0f;
		if (!ParseFloat(Trim(value.substr(0, comma)), parsedX))
			return false;
		if (!ParseFloat(Trim(value.substr(comma + 1)), parsedY))
			return false;
		x = parsedX;
		y = parsedY;
		return true;
	}

	std::vector<std::string> SplitList(const std::string& value)
	{
		std::vector<std::string> out;
		std::string item;
		for (char c : value)
		{
			if (c == ',' || c == ';')
			{
				item = Trim(item);
				if (!item.empty())
					out.push_back(item);
				item.clear();
			}
			else
			{
				item.push_back(c);
			}
		}
		item = Trim(item);
		if (!item.empty())
			out.push_back(item);
		return out;
	}

	bool ParsePointList(const std::string& value, std::vector<std::pair<float, float>>& out)
	{
		out.clear();
		std::vector<std::string> items;
		std::string item;
		for (char c : value)
		{
			if (c == ';')
			{
				item = Trim(item);
				if (!item.empty())
					items.push_back(item);
				item.clear();
			}
			else
			{
				item.push_back(c);
			}
		}
		item = Trim(item);
		if (!item.empty())
			items.push_back(item);

		for (const std::string& point : items)
		{
			float x = 0.0f;
			float y = 0.0f;
			if (!ParseFloatPair(point, x, y))
				return false;
			out.emplace_back(x, y);
		}
		return !out.empty();
	}

	bool IsLayoutNodeKind(const std::string& kind)
	{
		static const std::unordered_set<std::string> kKinds = {
			"region", "district", "block", "parcel", "building", "street",
			"road", "alley", "plaza", "park", "interior", "floor", "room",
			"corridor", "stair", "landmark", "gameplay", "streaming", "volume"
		};
		return kKinds.find(kind) != kKinds.end();
	}

	bool IsLayoutRoadKind(const std::string& kind)
	{
		return kind == "street" || kind == "road" || kind == "alley";
	}

	bool IsLayoutOpenPolylineKind(const std::string& kind)
	{
		return IsLayoutRoadKind(kind) || kind == "gameplay";
	}

	bool IsLayoutOverlayKind(const std::string& kind)
	{
		return kind == "streaming" || kind == "gameplay" || kind == "volume";
	}

	float LayoutPolygonArea(const std::vector<std::pair<float, float>>& points)
	{
		if (points.size() < 3)
			return 0.0f;
		double area = 0.0;
		for (size_t i = 0; i < points.size(); ++i)
		{
			const auto& a = points[i];
			const auto& b = points[(i + 1) % points.size()];
			area += static_cast<double>(a.first) * static_cast<double>(b.second) -
				static_cast<double>(b.first) * static_cast<double>(a.second);
		}
		return static_cast<float>(std::abs(area) * 0.5);
	}

	bool PointInPolygon(const ImVec2& point, const std::vector<ImVec2>& polygon)
	{
		bool inside = false;
		for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++)
		{
			const ImVec2& a = polygon[i];
			const ImVec2& b = polygon[j];
			const bool crosses = ((a.y > point.y) != (b.y > point.y)) &&
				(point.x < (b.x - a.x) * (point.y - a.y) / ((b.y - a.y) + 1.0e-6f) + a.x);
			if (crosses)
				inside = !inside;
		}
		return inside;
	}

	int LayoutDrawLayer(const std::string& kind)
	{
		if (kind == "region" || kind == "district" || kind == "block" || kind == "parcel")
			return 0;
		if (kind == "park" || kind == "plaza")
			return 1;
		if (IsLayoutRoadKind(kind))
			return 2;
		if (kind == "building" || kind == "landmark")
			return 3;
		if (kind == "interior" || kind == "floor" || kind == "room" || kind == "corridor" || kind == "stair")
			return 4;
		if (kind == "streaming" || kind == "gameplay" || kind == "volume")
			return 5;
		return 6;
	}

	ImU32 ColorForLayoutType(const std::string& type, bool selected, bool fill)
	{
		if (selected)
			return fill ? IM_COL32(255, 216, 92, 98) : IM_COL32(255, 226, 122, 255);
		if (type == "street" || type == "road")
			return fill ? IM_COL32(140, 150, 160, 72) : IM_COL32(198, 205, 210, 235);
		if (type == "alley")
			return fill ? IM_COL32(120, 126, 132, 58) : IM_COL32(165, 172, 178, 210);
		if (type == "building" || type == "landmark")
			return fill ? IM_COL32(74, 132, 210, 86) : IM_COL32(108, 170, 255, 235);
		if (type == "plaza")
			return fill ? IM_COL32(222, 186, 110, 76) : IM_COL32(236, 205, 137, 225);
		if (type == "park")
			return fill ? IM_COL32(73, 154, 105, 75) : IM_COL32(109, 205, 144, 225);
		if (type == "parcel")
			return fill ? IM_COL32(112, 101, 80, 28) : IM_COL32(168, 150, 112, 180);
		if (type == "block" || type == "district" || type == "region")
			return fill ? IM_COL32(74, 83, 96, 14) : IM_COL32(135, 150, 172, 170);
		if (type == "room" || type == "corridor" || type == "interior" || type == "floor" || type == "stair")
			return fill ? IM_COL32(145, 99, 182, 62) : IM_COL32(184, 132, 222, 210);
		if (type == "gameplay" || type == "volume" || type == "streaming")
			return fill ? IM_COL32(220, 94, 104, 10) : IM_COL32(245, 128, 138, 125);
		return fill ? IM_COL32(90, 104, 120, 50) : IM_COL32(190, 200, 210, 210);
	}

	bool IsLayoutPartitionKind(const std::string& kind)
	{
		return kind == "block";
	}

	bool LayoutPointInPolygon(float x, float y, const std::vector<std::pair<float, float>>& polygon)
	{
		if (polygon.size() < 3)
			return false;
		bool inside = false;
		for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++)
		{
			const auto& a = polygon[i];
			const auto& b = polygon[j];
			const bool crosses = ((a.second > y) != (b.second > y)) &&
				(x < (b.first - a.first) * (y - a.second) / ((b.second - a.second) + 1.0e-6f) + a.first);
			if (crosses)
				inside = !inside;
		}
		return inside;
	}

	float Cross2D(float ax, float ay, float bx, float by)
	{
		return ax * by - ay * bx;
	}

	bool LayoutSegmentsIntersect(
		const std::pair<float, float>& a0,
		const std::pair<float, float>& a1,
		const std::pair<float, float>& b0,
		const std::pair<float, float>& b1)
	{
		const float rX = a1.first - a0.first;
		const float rY = a1.second - a0.second;
		const float sX = b1.first - b0.first;
		const float sY = b1.second - b0.second;
		const float denom = Cross2D(rX, rY, sX, sY);
		if (std::abs(denom) < 1.0e-5f)
			return false;
		const float qpX = b0.first - a0.first;
		const float qpY = b0.second - a0.second;
		const float t = Cross2D(qpX, qpY, sX, sY) / denom;
		const float u = Cross2D(qpX, qpY, rX, rY) / denom;
		return t >= 0.0f && t <= 1.0f && u >= 0.0f && u <= 1.0f;
	}

	bool LayoutPolygonsOverlap(
		const std::vector<std::pair<float, float>>& a,
		const std::vector<std::pair<float, float>>& b)
	{
		if (a.size() < 3 || b.size() < 3)
			return false;

		float aMinX = std::numeric_limits<float>::max();
		float aMinY = std::numeric_limits<float>::max();
		float aMaxX = std::numeric_limits<float>::lowest();
		float aMaxY = std::numeric_limits<float>::lowest();
		float bMinX = std::numeric_limits<float>::max();
		float bMinY = std::numeric_limits<float>::max();
		float bMaxX = std::numeric_limits<float>::lowest();
		float bMaxY = std::numeric_limits<float>::lowest();
		for (const auto& p : a)
		{
			aMinX = std::min(aMinX, p.first);
			aMinY = std::min(aMinY, p.second);
			aMaxX = std::max(aMaxX, p.first);
			aMaxY = std::max(aMaxY, p.second);
		}
		for (const auto& p : b)
		{
			bMinX = std::min(bMinX, p.first);
			bMinY = std::min(bMinY, p.second);
			bMaxX = std::max(bMaxX, p.first);
			bMaxY = std::max(bMaxY, p.second);
		}
		if (aMaxX < bMinX || bMaxX < aMinX || aMaxY < bMinY || bMaxY < aMinY)
			return false;

		for (const auto& p : a)
		{
			if (LayoutPointInPolygon(p.first, p.second, b))
				return true;
		}
		for (const auto& p : b)
		{
			if (LayoutPointInPolygon(p.first, p.second, a))
				return true;
		}
		for (size_t i = 0; i < a.size(); ++i)
		{
			const auto& a0 = a[i];
			const auto& a1 = a[(i + 1) % a.size()];
			for (size_t j = 0; j < b.size(); ++j)
			{
				if (LayoutSegmentsIntersect(a0, a1, b[j], b[(j + 1) % b.size()]))
					return true;
			}
		}
		return false;
	}

	std::pair<float, float> LayoutPolygonCentroid(const std::vector<std::pair<float, float>>& points)
	{
		if (points.empty())
			return { 0.0f, 0.0f };
		double signedArea = 0.0;
		double cx = 0.0;
		double cy = 0.0;
		for (size_t i = 0; i < points.size(); ++i)
		{
			const auto& a = points[i];
			const auto& b = points[(i + 1) % points.size()];
			const double cross = static_cast<double>(a.first) * b.second - static_cast<double>(b.first) * a.second;
			signedArea += cross;
			cx += (static_cast<double>(a.first) + b.first) * cross;
			cy += (static_cast<double>(a.second) + b.second) * cross;
		}
		if (std::abs(signedArea) < 1.0e-6)
		{
			float sx = 0.0f;
			float sy = 0.0f;
			for (const auto& p : points)
			{
				sx += p.first;
				sy += p.second;
			}
			return { sx / points.size(), sy / points.size() };
		}
		const double inv = 1.0 / (3.0 * signedArea);
		return { static_cast<float>(cx * inv), static_cast<float>(cy * inv) };
	}

	std::vector<std::pair<float, float>> ClipLayoutPolygonHalfPlane(
		const std::vector<std::pair<float, float>>& input,
		float nx,
		float ny,
		float c,
		bool keepLess)
	{
		std::vector<std::pair<float, float>> output;
		if (input.empty())
			return output;
		output.reserve(input.size() + 2);
		auto side = [&](const std::pair<float, float>& p)
		{
			return p.first * nx + p.second * ny - c;
		};
		auto inside = [&](float s)
		{
			return keepLess ? s <= 1.0e-3f : s >= -1.0e-3f;
		};
		for (size_t i = 0; i < input.size(); ++i)
		{
			const auto& a = input[i];
			const auto& b = input[(i + 1) % input.size()];
			const float sa = side(a);
			const float sb = side(b);
			const bool inA = inside(sa);
			const bool inB = inside(sb);
			if (inA && inB)
			{
				output.push_back(b);
			}
			else if (inA != inB)
			{
				const float denom = sa - sb;
				const float t = std::abs(denom) > 1.0e-6f ? sa / denom : 0.0f;
				const std::pair<float, float> hit = {
					a.first + (b.first - a.first) * t,
					a.second + (b.second - a.second) * t
				};
				output.push_back(hit);
				if (!inA && inB)
					output.push_back(b);
			}
		}
		return output;
	}

	float LayoutPointDistanceSq(const std::pair<float, float>& a, const std::pair<float, float>& b)
	{
		const float dx = a.first - b.first;
		const float dy = a.second - b.second;
		return dx * dx + dy * dy;
	}

	void PushLayoutPointUnique(std::vector<std::pair<float, float>>& points, const std::pair<float, float>& p)
	{
		if (!points.empty() && LayoutPointDistanceSq(points.back(), p) < 1.0f)
			return;
		points.push_back(p);
	}

	void SimplifyLayoutPolygon(std::vector<std::pair<float, float>>& points)
	{
		if (points.size() < 3)
			return;
		std::vector<std::pair<float, float>> compact;
		compact.reserve(points.size());
		for (const auto& p : points)
			PushLayoutPointUnique(compact, p);
		if (compact.size() > 1 && LayoutPointDistanceSq(compact.front(), compact.back()) < 1.0f)
			compact.pop_back();
		points = std::move(compact);
	}

	void AddLayoutTValue(std::vector<float>& values, float t)
	{
		for (float existing : values)
		{
			if (std::abs(existing - t) < 1.0f)
				return;
		}
		values.push_back(t);
	}

	std::vector<float> SortedLayoutTValues(std::vector<float> values, float minT, float maxT)
	{
		std::vector<float> out;
		out.reserve(values.size() + 2);
		AddLayoutTValue(out, minT);
		AddLayoutTValue(out, maxT);
		for (float t : values)
		{
			if (t > minT + 1.0f && t < maxT - 1.0f)
				AddLayoutTValue(out, t);
		}
		std::sort(out.begin(), out.end());
		return out;
	}

	bool InsertSharedLineVerticesIntoPolygon(
		std::vector<std::pair<float, float>>& polygon,
		float tx,
		float ty,
		float nx,
		float ny,
		float sharedC,
		const std::vector<float>& sharedTValues,
		float lineTolerance)
	{
		if (polygon.size() < 3 || sharedTValues.size() < 2)
			return false;

		bool changed = false;
		std::vector<std::pair<float, float>> out;
		out.reserve(polygon.size() + sharedTValues.size());
		for (size_t i = 0; i < polygon.size(); ++i)
		{
			const auto& a = polygon[i];
			const auto& b = polygon[(i + 1) % polygon.size()];
			PushLayoutPointUnique(out, a);

			const float da = std::abs(a.first * nx + a.second * ny - sharedC);
			const float db = std::abs(b.first * nx + b.second * ny - sharedC);
			if (da > lineTolerance || db > lineTolerance)
				continue;

			const float aT = a.first * tx + a.second * ty;
			const float bT = b.first * tx + b.second * ty;
			const float edgeMinT = std::min(aT, bT);
			const float edgeMaxT = std::max(aT, bT);
			if (edgeMaxT - edgeMinT < 1.0f)
				continue;

			std::vector<float> edgeT;
			for (float t : sharedTValues)
			{
				if (t > edgeMinT + 1.0f && t < edgeMaxT - 1.0f)
					edgeT.push_back(t);
			}
			if (edgeT.empty())
				continue;
			if (aT > bT)
				std::reverse(edgeT.begin(), edgeT.end());
			for (float t : edgeT)
			{
				PushLayoutPointUnique(out, { tx * t + nx * sharedC, ty * t + ny * sharedC });
				changed = true;
			}
		}
		SimplifyLayoutPolygon(out);
		if (out.size() < 3 || LayoutPolygonArea(out) < 100.0f)
			return false;
		if (changed)
			polygon = std::move(out);
		return changed;
	}

	bool SnapLayoutPolygonEdgeToSharedLine(
		std::vector<std::pair<float, float>>& polygon,
		size_t edgeIndex,
		float tx,
		float ty,
		float nx,
		float ny,
		float sharedC,
		float overlapMinT,
		float overlapMaxT,
		const std::vector<float>& sharedTValues)
	{
		if (polygon.size() < 3 || edgeIndex >= polygon.size() || overlapMaxT - overlapMinT < 1.0f)
			return false;
		const size_t edgeNext = (edgeIndex + 1) % polygon.size();
		const auto edgeStart = polygon[edgeIndex];
		const auto edgeEnd = polygon[edgeNext];
		const float startT = edgeStart.first * tx + edgeStart.second * ty;
		const float endT = edgeEnd.first * tx + edgeEnd.second * ty;
		const bool forward = startT <= endT;
		const float firstT = forward ? overlapMinT : overlapMaxT;
		const float secondT = forward ? overlapMaxT : overlapMinT;
		const float minT = std::min(overlapMinT, overlapMaxT);
		const float maxT = std::max(overlapMinT, overlapMaxT);
		std::vector<float> sortedT = SortedLayoutTValues(sharedTValues, minT, maxT);
		if (!forward)
			std::reverse(sortedT.begin(), sortedT.end());

		auto snapPointIfInside = [&](const std::pair<float, float>& p) -> std::pair<float, float>
		{
			const float t = p.first * tx + p.second * ty;
			if (t >= minT - 1.0f && t <= maxT + 1.0f)
				return { tx * t + nx * sharedC, ty * t + ny * sharedC };
			return p;
		};

		std::vector<std::pair<float, float>> out;
		out.reserve(polygon.size() + sortedT.size());
		for (size_t i = 0; i < polygon.size(); ++i)
		{
			const bool edgeEndpoint = i == edgeIndex || i == edgeNext;
			PushLayoutPointUnique(out, edgeEndpoint ? snapPointIfInside(polygon[i]) : polygon[i]);
			if (i == edgeIndex)
			{
				for (float t : sortedT)
					PushLayoutPointUnique(out, { tx * t + nx * sharedC, ty * t + ny * sharedC });
				PushLayoutPointUnique(out, { tx * secondT + nx * sharedC, ty * secondT + ny * sharedC });
			}
		}
		SimplifyLayoutPolygon(out);
		if (out.size() < 3 || LayoutPolygonArea(out) < 100.0f)
			return false;
		polygon = std::move(out);
		return true;
	}

	std::string FormatLayoutFloat(float value)
	{
		if (std::abs(value) < 0.0005f)
			value = 0.0f;
		std::ostringstream ss;
		ss << std::fixed << std::setprecision(3) << value;
		std::string out = ss.str();
		while (out.size() > 1 && out.back() == '0')
			out.pop_back();
		if (!out.empty() && out.back() == '.')
			out.pop_back();
		return out;
	}

	std::string FormatLayoutPointList(const std::vector<std::pair<float, float>>& points)
	{
		std::ostringstream ss;
		for (size_t i = 0; i < points.size(); ++i)
		{
			if (i > 0)
				ss << ';';
			ss << '(' << FormatLayoutFloat(points[i].first) << ',' << FormatLayoutFloat(points[i].second) << ')';
		}
		return ss.str();
	}

	std::string EscapeLayoutValue(const std::string& value)
	{
		if (value.empty())
			return "\"\"";
		bool needsQuote = false;
		for (char c : value)
		{
			if (std::isspace(static_cast<unsigned char>(c)) || c == '#' || c == ';' || c == '"')
			{
				needsQuote = true;
				break;
			}
		}
		if (!needsQuote)
			return value;
		std::string out = "\"";
		for (char c : value)
		{
			if (c == '"' || c == '\\')
				out.push_back('\\');
			out.push_back(c);
		}
		out.push_back('"');
		return out;
	}

	std::string EscapeLuaStringForMap(const std::string& value)
	{
		std::string out = "\"";
		for (char c : value)
		{
			switch (c)
			{
			case '\\': out += "\\\\"; break;
			case '"': out += "\\\""; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default: out.push_back(c); break;
			}
		}
		out.push_back('"');
		return out;
	}
}

CoronaAssetExplorer::CoronaAssetExplorer(Corona* host)
	: Host(host)
{
	AssetsRoot = RuntimePaths::RootDirectory() / L"assets";
	std::error_code ec;
	std::filesystem::create_directories(AssetsRoot, ec);
	CurrentDir = AssetsRoot;
}

CoronaAssetExplorer::~CoronaAssetExplorer() = default;

void CoronaAssetExplorer::RefreshIfStale()
{
	using clock = std::chrono::steady_clock;
	const auto now = clock::now();
	if (!CurrentListing.empty() && (now - LastRefresh) < std::chrono::milliseconds(500))
		return;
	LastRefresh = now;

	CurrentListing.clear();
	std::error_code ec;
	if (!std::filesystem::exists(CurrentDir, ec))
		CurrentDir = AssetsRoot;
	for (const auto& it : std::filesystem::directory_iterator(CurrentDir, ec))
	{
		Entry e;
		e.Path = it.path();
		e.bIsDirectory = it.is_directory(ec);
		CurrentListing.push_back(std::move(e));
	}
	// Directories first, then alpha-sorted.
	std::sort(CurrentListing.begin(), CurrentListing.end(),
		[](const Entry& a, const Entry& b)
		{
			if (a.bIsDirectory != b.bIsDirectory)
				return a.bIsDirectory > b.bIsDirectory;
			return a.Path.filename() < b.Path.filename();
		});
}

void CoronaAssetExplorer::RenderImGui()
{
	DrawBottomToggleButton();
	RenderWorldLayoutViewer();
	RenderRoadGraphViewer();
	RenderCityLayoutViewer();
	if (!bVisible)
		return;

	RefreshIfStale();

	const ImGuiViewport* vp = ImGui::GetMainViewport();
	const float h = vp->WorkSize.y;
	// Dock to the left edge, mirroring the scene inspector panel. The asset
	// panel auto-closes after a load/spawn, so overlapping the inspector while
	// open is fine.
	ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y));
	ImGui::SetNextWindowSize(ImVec2(kPanelWidth + 40.0f, h - kBottomBarH - 4.0f));

	const ImGuiWindowFlags flags =
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoSavedSettings;
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.06f, 0.09f, 0.94f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::Begin("##asset_explorer", nullptr, flags);

	ImGui::TextColored(ImVec4(1.0f, 0.92f, 0.85f, 1.0f), "Asset Explorer");
	const std::string relDir = std::filesystem::relative(CurrentDir, AssetsRoot).string();
	ImGui::TextDisabled("assets/%s", relDir.empty() ? "" : (relDir + "/").c_str());
	ImGui::Separator();

	if (CurrentDir != AssetsRoot)
	{
		if (ImGui::Selectable(".. (up)"))
		{
			CurrentDir = CurrentDir.parent_path();
			CurrentListing.clear(); // force refresh next frame
		}
	}

	for (const auto& entry : CurrentListing)
		RenderEntry(entry);

	if (!PendingErrorMessage.empty())
	{
		ImGui::Separator();
		ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "%s", PendingErrorMessage.c_str());
	}

	ImGui::End();
	ImGui::PopStyleVar();
	ImGui::PopStyleColor();

	// Apply a queued double-click action now that we're done iterating the
	// listing (these mutate CurrentListing / bVisible).
	const PendingAction action = QueuedAction;
	const std::filesystem::path actionPath = QueuedActionPath;
	QueuedAction = PendingAction::None;
	QueuedActionPath.clear();
	switch (action)
	{
	case PendingAction::Navigate:
		CurrentDir = actionPath;
		CurrentListing.clear();
		break;
	case PendingAction::LoadMap:
		LoadMapAsset(actionPath);
		break;
	case PendingAction::SpawnMesh:
		SpawnMeshFromAsset(actionPath);
		break;
	case PendingAction::OpenWorldLayout:
		OpenWorldLayoutAsset(actionPath);
		break;
	case PendingAction::OpenRoadGraph:
		OpenRoadGraphAsset(actionPath);
		break;
	case PendingAction::OpenCityLayout:
		OpenCityLayoutAsset(actionPath);
		break;
	case PendingAction::None:
	default:
		break;
	}
}

void CoronaAssetExplorer::RenderEntry(const Entry& entry)
{
	const std::string label = (entry.bIsDirectory ? std::string("[D] ") : std::string("    "))
		+ entry.Path.filename().string();

	const std::string popupId = "asset_ctx##" + entry.Path.filename().string();

	if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick))
	{
		if (ImGui::IsMouseDoubleClicked(0))
		{
			// Defer: navigating / loading mutates the listing we're iterating.
			if (entry.bIsDirectory)
			{
				QueuedAction = PendingAction::Navigate;
				QueuedActionPath = entry.Path;
			}
			else if (IsMapFile(entry.Path))
			{
				QueuedAction = PendingAction::LoadMap;
				QueuedActionPath = entry.Path;
			}
			else if (IsWorldLayoutFile(entry.Path))
			{
				QueuedAction = PendingAction::OpenWorldLayout;
				QueuedActionPath = entry.Path;
			}
			else if (IsRoadGraphFile(entry.Path))
			{
				QueuedAction = PendingAction::OpenRoadGraph;
				QueuedActionPath = entry.Path;
			}
			else if (IsCityLayoutFile(entry.Path))
			{
				QueuedAction = PendingAction::OpenCityLayout;
				QueuedActionPath = entry.Path;
			}
			else if (IsSpawnableMesh(entry.Path))
			{
				QueuedAction = PendingAction::SpawnMesh;
				QueuedActionPath = entry.Path;
			}
		}
	}
	if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(1))
		ImGui::OpenPopup(popupId.c_str());

	HandleContextMenu(entry);
}

void CoronaAssetExplorer::HandleContextMenu(const Entry& entry)
{
	const std::string popupId = "asset_ctx##" + entry.Path.filename().string();
	if (!ImGui::BeginPopup(popupId.c_str()))
		return;

	const std::filesystem::path path = entry.Path;

	if (entry.bIsDirectory)
	{
		if (ImGui::MenuItem("Open"))
		{
			CurrentDir = path;
			CurrentListing.clear();
		}
	}
	else
	{
		if (IsModelFile(path))
		{
			// Sibling .cmesh cache (if any). Showing the menu only when
			// the cache actually exists keeps the UI honest — without
			// a cache there's nothing to invalidate.
			const std::filesystem::path cachePath = MeshCachePathFor(path);
			std::error_code cacheEc;
			const bool bCacheExists = std::filesystem::exists(cachePath, cacheEc);
			if (bCacheExists)
			{
				if (ImGui::MenuItem("Re-import (rebuild .cmesh)"))
				{
					std::wstring err;
					if (!Host->InvalidateMeshCacheForSource(path.wstring(), &err))
						PendingErrorMessage = "invalidate failed: " + PlatformWideToUtf8(err);
					else
					{
						PendingErrorMessage = "cache cleared — re-import on next load";
						CurrentListing.clear();
					}
				}
			}
			if (ImGui::MenuItem("Spawn model"))
				SpawnMeshFromAsset(path);
		}
		if (IsMapFile(path))
		{
			if (ImGui::MenuItem("Load map (replace scene)"))
				LoadMapAsset(path);
		}
		if (IsWorldLayoutFile(path))
		{
			if (ImGui::MenuItem("Open world layout graph"))
				OpenWorldLayoutAsset(path);
		}
		if (IsRoadGraphFile(path))
		{
			if (ImGui::MenuItem("Open road graph"))
				OpenRoadGraphAsset(path);
		}
		if (IsCityLayoutFile(path))
		{
			if (ImGui::MenuItem("Open city layout"))
				OpenCityLayoutAsset(path);
		}
		if (IsImageFile(path))
		{
			ImGui::TextDisabled("(image — use console `generate <path>`)");
		}
		if (IsMeshCacheFile(path))
		{
			// .cmesh files are derived; deleting forces a rebuild from
			// the sibling source asset on next load.
			if (ImGui::MenuItem("Delete cache (re-import on next load)"))
			{
				std::error_code ec;
				std::filesystem::remove(path, ec);
				if (ec)
					PendingErrorMessage = "delete failed: " + ec.message();
				else
				{
					PendingErrorMessage = "cache cleared";
					CurrentListing.clear();
					AppendCpuRuntimeTrace(L"[AssetExplorer] cleared cmesh cache " + path.wstring());
				}
			}
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Delete file"))
		{
			std::error_code ec;
			std::filesystem::remove(path, ec);
			if (ec)
				PendingErrorMessage = "delete failed: " + ec.message();
			else
			{
				PendingErrorMessage.clear();
				CurrentListing.clear();
				AppendCpuRuntimeTrace(L"[AssetExplorer] deleted " + path.wstring());
			}
		}
	}

	ImGui::EndPopup();
}

void CoronaAssetExplorer::SpawnMeshFromAsset(const std::filesystem::path& path)
{
	const Corona::ScriptSceneHandle sh = Host->LoadSceneForScript(path.wstring());
	if (sh == Corona::InvalidScriptSceneHandle)
	{
		PendingErrorMessage = "LoadSceneForScript failed: " + path.string();
		return;
	}

	// Camera-forward raycast: spawn at the midpoint between the camera and the
	// first world hit so the model lands on visible geometry. World units are
	// centimetres (1 m = 100 units; camera near/far = 10 / 20000), so the
	// no-hit fallback of 5 m ahead is 500 units.
	constexpr float kMetersToWorld = 100.0f;
	const glm::vec3 camPos = Host->GetCameraPositionForConsole();
	glm::vec3 camLook = Host->GetCameraLookDirForConsole();
	const float lookLen = glm::length(camLook);
	camLook = (lookLen > 1.0e-6f) ? (camLook / lookLen) : glm::vec3(0.0f, 0.0f, 1.0f);

	glm::vec3 spawnPos;
	Corona::CpuPhysicsRaycastHit hit;
	if (Host->CpuPhysicsRaycast(camPos, camLook, 200.0f * kMetersToWorld, hit))
		spawnPos = (camPos + hit.Position) * 0.5f;
	else
		spawnPos = camPos + camLook * (5.0f * kMetersToWorld);

	const std::string name = "Spawn_" + path.stem().string();
	CoronaECS::Entity e = Host->CreateEntity(name);
	// Natural mesh scale (useScale=true, scale=1): UE-exported assets are
	// authored at world scale, so this shows them at their real size.
	Host->AddMeshComponentForScript(
		e, sh, spawnPos, glm::vec3(0.0f),
		/*targetExtent*/ 1.0f, glm::vec3(1.0f), /*useScale*/ true,
		/*roughness*/ 0.6f, /*metallic*/ 0.0f,
		/*overrideRM*/ true,
		/*visible*/ true, /*rayTracing*/ true, /*physicsQuery*/ true);

	PendingErrorMessage.clear();
	AppendCpuRuntimeTrace(L"[AssetExplorer] spawned " + path.filename().wstring());
	bVisible = false; // close after spawning
}

void CoronaAssetExplorer::LoadMapAsset(const std::filesystem::path& path)
{
	// `.map` is the canonical extension; ResolveMapPath adds it when the name
	// has no dot, so we pass just the stem.
	Host->QueueEditorMapLoad(path.stem().wstring());
	PendingErrorMessage.clear();
	AppendCpuRuntimeTrace(L"[AssetExplorer] loadmap queued " + path.stem().wstring());
	bVisible = false; // close after queueing the load
}

void CoronaAssetExplorer::OpenWorldLayoutAsset(const std::filesystem::path& path)
{
	WorldLayoutDocument loaded;
	loaded.SourcePath = path;
	loaded.Name = path.stem().string();

	std::ifstream file(path);
	if (!file)
	{
		PendingErrorMessage = "open failed: " + path.string();
		loaded.Error = PendingErrorMessage;
		LayoutDocument = std::move(loaded);
		bWorldLayoutViewerVisible = true;
		return;
	}

	std::unordered_map<std::string, size_t> nodeIndexById;
	std::string line;
	int lineNumber = 0;
	while (std::getline(file, line))
	{
		++lineNumber;
		line = Trim(StripComment(line));
		if (line.empty())
			continue;

		std::vector<std::string> tokens = TokenizeLayoutLine(line);
		if (tokens.empty())
			continue;

		std::string kind = tokens[0];
		std::transform(kind.begin(), kind.end(), kind.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		const auto values = ParseKeyValues(tokens, 1);

		if (kind == "world")
		{
			auto nameIt = values.find("name");
			if (nameIt != values.end() && !nameIt->second.empty())
				loaded.Name = nameIt->second;
			continue;
		}

		if (kind == "link" || kind == "edge")
		{
			WorldLayoutEdge edge;
			auto fromIt = values.find("from");
			auto toIt = values.find("to");
			auto typeIt = values.find("type");
			if (fromIt != values.end())
				edge.From = fromIt->second;
			else if (tokens.size() > 1 && tokens[1].find('=') == std::string::npos)
				edge.From = tokens[1];
			if (toIt != values.end())
				edge.To = toIt->second;
			else if (tokens.size() > 2 && tokens[2].find('=') == std::string::npos)
				edge.To = tokens[2];
			edge.Type = (typeIt != values.end()) ? typeIt->second : "link";
			if (!edge.From.empty() && !edge.To.empty())
				loaded.Edges.push_back(std::move(edge));
			else
				loaded.Error += "line " + std::to_string(lineNumber) + ": link requires from/to\n";
			continue;
		}

		if (!IsLayoutNodeKind(kind))
		{
			loaded.Error += "line " + std::to_string(lineNumber) + ": unknown node type '" + kind + "'\n";
			continue;
		}

		WorldLayoutNode node;
		node.Type = kind;
		auto idIt = values.find("id");
		node.Id = (idIt != values.end() && !idIt->second.empty()) ?
			idIt->second :
			(kind + "_" + std::to_string(loaded.Nodes.size() + 1));
		if (nodeIndexById.find(node.Id) != nodeIndexById.end())
		{
			loaded.Error += "line " + std::to_string(lineNumber) + ": duplicate id '" + node.Id + "'\n";
			continue;
		}

		auto labelIt = values.find("label");
		auto nameIt = values.find("name");
		node.Label = (labelIt != values.end()) ? labelIt->second :
			((nameIt != values.end()) ? nameIt->second : node.Id);
		auto parentIt = values.find("parent");
		if (parentIt != values.end())
			node.Parent = parentIt->second;

		auto posIt = values.find("pos");
		if (posIt == values.end())
			posIt = values.find("center");
		if (posIt != values.end() && ParseFloatPair(posIt->second, node.X, node.Y))
			node.bHasPosition = true;

		auto sizeIt = values.find("size");
		if (sizeIt != values.end() && ParseFloatPair(sizeIt->second, node.Width, node.Height2D))
			node.bHasSize = true;

		auto polygonIt = values.find("polygon");
		if (polygonIt == values.end())
			polygonIt = values.find("footprint");
		if (polygonIt == values.end())
			polygonIt = values.find("boundary");
		if (polygonIt != values.end())
		{
			if (!ParsePointList(polygonIt->second, node.Points))
				loaded.Error += "line " + std::to_string(lineNumber) + ": invalid polygon list\n";
			else
				node.bClosedPolygon = true;
		}
		else
		{
			auto pointsIt = values.find("points");
			if (pointsIt != values.end())
			{
				if (!ParsePointList(pointsIt->second, node.Points))
					loaded.Error += "line " + std::to_string(lineNumber) + ": invalid points list\n";
				else
					node.bClosedPolygon = !IsLayoutOpenPolylineKind(node.Type) && node.Points.size() >= 3;
			}
		}

		if (!node.bHasSize)
		{
			node.Width = ParseFloatOr(values, "width", node.Width);
			node.Height2D = ParseFloatOr(values, "depth", node.Height2D);
			node.bHasSize = node.Width > 0.0f && node.Height2D > 0.0f;
		}
		node.BuildingHeight = ParseFloatOr(values, "height", 0.0f);
		node.Floors = ParseIntOr(values, "floors", 0);

		for (const auto& kv : values)
		{
			if (kv.first == "id" || kv.first == "label" || kv.first == "name" ||
				kv.first == "parent" || kv.first == "pos" || kv.first == "center" ||
				kv.first == "size" || kv.first == "points" || kv.first == "polygon" ||
				kv.first == "footprint" || kv.first == "boundary")
			{
				continue;
			}
			node.Attributes.push_back({ kv.first, kv.second });
		}

		nodeIndexById[node.Id] = loaded.Nodes.size();
		loaded.Nodes.push_back(std::move(node));
	}

	for (const WorldLayoutNode& node : loaded.Nodes)
	{
		if (!node.Parent.empty())
			loaded.Edges.push_back({ node.Parent, node.Id, "contains" });

		for (const WorldLayoutAttribute& attr : node.Attributes)
		{
			if (attr.Key != "entrances" && attr.Key != "connects" && attr.Key != "links")
				continue;
			for (const std::string& target : SplitList(attr.Value))
			{
				if (!target.empty())
					loaded.Edges.push_back({ node.Id, target, attr.Key });
			}
		}
	}

	loaded.bLoaded = true;
	LayoutDocument = std::move(loaded);
	RemoveGeneratedOverlapRoadsForNode(std::string());
	KeepWorldLayoutBlocksAndBuildings();
	for (int pass = 0; pass < 3; ++pass)
	{
		bool alignedAny = false;
		for (int i = 0; i < static_cast<int>(LayoutDocument.Nodes.size()); ++i)
			alignedAny = AlignWorldLayoutAdjacentEdgesForNode(i) || alignedAny;
		if (!alignedAny)
			break;
	}
	SelectedWorldLayoutNode = LayoutDocument.Nodes.empty() ? -1 : 0;
	DraggingWorldLayoutNode = -1;
	DraggingWorldLayoutVertex = -1;
	bWorldLayoutDragMoved = false;
	bWorldLayoutDirty = false;
	bWorldLayoutViewerVisible = true;
	bWorldLayoutAutoFit = true;
	WorldLayoutZoom = 1.0f;
	WorldLayoutPanX = 0.0f;
	WorldLayoutPanY = 0.0f;
	PendingErrorMessage.clear();
	AppendCpuRuntimeTrace(L"[AssetExplorer] opened world layout " + path.wstring());
}

int CoronaAssetExplorer::FindWorldLayoutNodeIndex(const std::string& id) const
{
	for (int i = 0; i < static_cast<int>(LayoutDocument.Nodes.size()); ++i)
	{
		if (LayoutDocument.Nodes[i].Id == id)
			return i;
	}
	return -1;
}

bool CoronaAssetExplorer::IsWorldLayoutDescendantOf(int nodeIndex, const std::string& ancestorId) const
{
	if (nodeIndex < 0 || nodeIndex >= static_cast<int>(LayoutDocument.Nodes.size()) || ancestorId.empty())
		return false;
	std::string parent = LayoutDocument.Nodes[nodeIndex].Parent;
	std::unordered_set<std::string> visited;
	while (!parent.empty())
	{
		if (parent == ancestorId)
			return true;
		if (!visited.insert(parent).second)
			return false;
		const int parentIndex = FindWorldLayoutNodeIndex(parent);
		if (parentIndex < 0)
			return false;
		parent = LayoutDocument.Nodes[parentIndex].Parent;
	}
	return false;
}

void CoronaAssetExplorer::KeepWorldLayoutBlocksAndBuildings()
{
	std::unordered_map<std::string, int> oldIndexById;
	oldIndexById.reserve(LayoutDocument.Nodes.size());
	for (int i = 0; i < static_cast<int>(LayoutDocument.Nodes.size()); ++i)
		oldIndexById[LayoutDocument.Nodes[i].Id] = i;

	auto blockAncestorChain = [&](const WorldLayoutNode& node)
	{
		std::vector<std::string> chain;
		std::string parent = node.Parent;
		std::unordered_set<std::string> visited;
		while (!parent.empty() && visited.insert(parent).second)
		{
			auto it = oldIndexById.find(parent);
			if (it == oldIndexById.end())
				break;
			const WorldLayoutNode& parentNode = LayoutDocument.Nodes[it->second];
			if (parentNode.Type == "block")
				chain.push_back(parentNode.Id);
			parent = parentNode.Parent;
		}
		std::reverse(chain.begin(), chain.end());
		return chain;
	};

	std::unordered_map<std::string, int> keptBlockDepth;
	std::unordered_set<std::string> keptIds;
	for (const WorldLayoutNode& node : LayoutDocument.Nodes)
	{
		if (node.Type != "block" && node.Type != "building")
			continue;
		const std::vector<std::string> ancestors = blockAncestorChain(node);
		const int depth = std::min(static_cast<int>(ancestors.size()), 1);
		keptBlockDepth[node.Id] = depth;
		keptIds.insert(node.Id);
	}

	std::vector<WorldLayoutNode> nodes;
	nodes.reserve(LayoutDocument.Nodes.size());
	for (const WorldLayoutNode& node : LayoutDocument.Nodes)
	{
		if (node.Type != "block" && node.Type != "building")
			continue;

		WorldLayoutNode kept = node;
		kept.Attributes.erase(std::remove_if(kept.Attributes.begin(), kept.Attributes.end(),
			[](const WorldLayoutAttribute& attr)
			{
				return attr.Key == "entrances" || attr.Key == "connects" || attr.Key == "links" ||
					attr.Key == "generated" || attr.Key == "between";
			}), kept.Attributes.end());

		const std::vector<std::string> ancestors = blockAncestorChain(node);
		if (kept.Type == "block")
		{
			if (ancestors.empty())
			{
				kept.Parent.clear();
			}
			else if (ancestors.size() == 1)
			{
				kept.Parent = ancestors[0];
			}
			else
			{
				kept.Parent = ancestors.front();
			}
		}
		else
		{
			kept.Parent.clear();
			for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it)
			{
				if (keptIds.find(*it) != keptIds.end())
				{
					kept.Parent = *it;
					break;
				}
			}
		}
		nodes.push_back(std::move(kept));
	}

	for (WorldLayoutNode& node : nodes)
	{
		if (!node.Parent.empty() && keptIds.find(node.Parent) == keptIds.end())
			node.Parent.clear();
		if (node.Type == "block")
		{
			auto it = keptBlockDepth.find(node.Id);
			if (it != keptBlockDepth.end() && it->second == 0)
				node.Parent.clear();
		}
	}

	LayoutDocument.Nodes = std::move(nodes);
	LayoutDocument.Edges.clear();
	for (const WorldLayoutNode& node : LayoutDocument.Nodes)
	{
		if (!node.Parent.empty())
			LayoutDocument.Edges.push_back({ node.Parent, node.Id, "contains" });
	}
}

void CoronaAssetExplorer::MoveWorldLayoutNodeWithChildren(int nodeIndex, float dx, float dy)
{
	if (nodeIndex < 0 || nodeIndex >= static_cast<int>(LayoutDocument.Nodes.size()))
		return;
	const std::string rootId = LayoutDocument.Nodes[nodeIndex].Id;
	for (int i = 0; i < static_cast<int>(LayoutDocument.Nodes.size()); ++i)
	{
		if (i != nodeIndex && !IsWorldLayoutDescendantOf(i, rootId))
			continue;
		WorldLayoutNode& node = LayoutDocument.Nodes[i];
		if (node.bHasPosition)
		{
			node.X += dx;
			node.Y += dy;
		}
		for (auto& p : node.Points)
		{
			p.first += dx;
			p.second += dy;
		}
	}
}

void CoronaAssetExplorer::RemoveGeneratedOverlapRoadsForNode(const std::string& nodeId)
{
	std::unordered_set<std::string> removedIds;
	for (int i = static_cast<int>(LayoutDocument.Nodes.size()) - 1; i >= 0; --i)
	{
		const WorldLayoutNode& node = LayoutDocument.Nodes[i];
		bool generatedOverlapRoad = false;
		bool referencesNode = false;
		for (const WorldLayoutAttribute& attr : node.Attributes)
		{
			if (attr.Key == "generated" && attr.Value == "overlap_split_road")
				generatedOverlapRoad = true;
			if (attr.Key == "between")
			{
				for (const std::string& id : SplitList(attr.Value))
				{
					if (id == nodeId)
						referencesNode = true;
				}
			}
		}
		if (nodeId.empty())
			referencesNode = true;
		if (generatedOverlapRoad && referencesNode)
		{
			removedIds.insert(node.Id);
			LayoutDocument.Nodes.erase(LayoutDocument.Nodes.begin() + i);
		}
	}
	if (removedIds.empty())
		return;
	LayoutDocument.Edges.erase(std::remove_if(LayoutDocument.Edges.begin(), LayoutDocument.Edges.end(),
		[&](const WorldLayoutEdge& edge)
		{
			return removedIds.find(edge.From) != removedIds.end() || removedIds.find(edge.To) != removedIds.end();
		}), LayoutDocument.Edges.end());
}

bool CoronaAssetExplorer::ResolveWorldLayoutOverlapsForNode(int nodeIndex)
{
	if (nodeIndex < 0 || nodeIndex >= static_cast<int>(LayoutDocument.Nodes.size()))
		return false;
	const std::string movedId = LayoutDocument.Nodes[nodeIndex].Id;
	RemoveGeneratedOverlapRoadsForNode(movedId);
	nodeIndex = FindWorldLayoutNodeIndex(movedId);
	if (nodeIndex < 0 || nodeIndex >= static_cast<int>(LayoutDocument.Nodes.size()))
		return false;

	auto nodePolygon = [](const WorldLayoutNode& node, std::vector<std::pair<float, float>>& out) -> bool
	{
		out.clear();
		if (node.bClosedPolygon && node.Points.size() >= 3)
		{
			out = node.Points;
			return true;
		}
		if (node.bHasSize)
		{
			const float x0 = node.X - node.Width * 0.5f;
			const float x1 = node.X + node.Width * 0.5f;
			const float y0 = node.Y - node.Height2D * 0.5f;
			const float y1 = node.Y + node.Height2D * 0.5f;
			out = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
			return true;
		}
		return false;
	};
	auto setNodePolygon = [&](int i, const std::vector<std::pair<float, float>>& polygon)
	{
		WorldLayoutNode& node = LayoutDocument.Nodes[i];
		node.Points = polygon;
		node.bClosedPolygon = true;
		node.bHasPosition = false;
		node.bHasSize = false;
	};
	WorldLayoutNode& movedNode = LayoutDocument.Nodes[nodeIndex];
	if (!IsLayoutPartitionKind(movedNode.Type))
		return false;
	std::vector<std::pair<float, float>> movedPoly;
	if (!nodePolygon(movedNode, movedPoly))
		return false;

	bool changed = false;
	const std::string parent = movedNode.Parent;
	for (int otherIndex = 0; otherIndex < static_cast<int>(LayoutDocument.Nodes.size()); ++otherIndex)
	{
		nodeIndex = FindWorldLayoutNodeIndex(movedId);
		if (nodeIndex < 0 || otherIndex == nodeIndex || otherIndex >= static_cast<int>(LayoutDocument.Nodes.size()))
			continue;
		WorldLayoutNode& other = LayoutDocument.Nodes[otherIndex];
		if (other.Parent != parent || !IsLayoutPartitionKind(other.Type))
			continue;
		std::vector<std::pair<float, float>> otherPoly;
		if (!nodePolygon(other, otherPoly) || !LayoutPolygonsOverlap(movedPoly, otherPoly))
			continue;

		const auto movedCentroid = LayoutPolygonCentroid(movedPoly);
		const auto otherCentroid = LayoutPolygonCentroid(otherPoly);
		float nx = otherCentroid.first - movedCentroid.first;
		float ny = otherCentroid.second - movedCentroid.second;
		const float len = std::sqrt(nx * nx + ny * ny);
		if (len < 1.0e-3f)
			continue;
		nx /= len;
		ny /= len;
		const float midX = (movedCentroid.first + otherCentroid.first) * 0.5f;
		const float midY = (movedCentroid.second + otherCentroid.second) * 0.5f;
		const float c = midX * nx + midY * ny;
		const bool movedKeepLess = (movedCentroid.first * nx + movedCentroid.second * ny) <= c;
		std::vector<std::pair<float, float>> clippedMoved = ClipLayoutPolygonHalfPlane(movedPoly, nx, ny, c, movedKeepLess);
		std::vector<std::pair<float, float>> clippedOther = ClipLayoutPolygonHalfPlane(otherPoly, nx, ny, c, !movedKeepLess);
		if (clippedMoved.size() < 3 || clippedOther.size() < 3 ||
			LayoutPolygonArea(clippedMoved) < 100.0f || LayoutPolygonArea(clippedOther) < 100.0f)
		{
			continue;
		}

		const float tx = -ny;
		const float ty = nx;
		std::vector<float> sharedTValues;
		auto collectBoundaryT = [&](const std::vector<std::pair<float, float>>& poly)
		{
			for (const auto& p : poly)
			{
				if (std::abs(p.first * nx + p.second * ny - c) <= 16.0f)
					AddLayoutTValue(sharedTValues, p.first * tx + p.second * ty);
			}
		};
		collectBoundaryT(clippedMoved);
		collectBoundaryT(clippedOther);
		if (sharedTValues.size() >= 2)
		{
			std::sort(sharedTValues.begin(), sharedTValues.end());
			const float boundaryMinT = sharedTValues.front();
			const float boundaryMaxT = sharedTValues.back();
			sharedTValues = SortedLayoutTValues(sharedTValues, boundaryMinT, boundaryMaxT);
			InsertSharedLineVerticesIntoPolygon(clippedMoved, tx, ty, nx, ny, c, sharedTValues, 16.0f);
			InsertSharedLineVerticesIntoPolygon(clippedOther, tx, ty, nx, ny, c, sharedTValues, 16.0f);
		}

		setNodePolygon(nodeIndex, clippedMoved);
		setNodePolygon(otherIndex, clippedOther);
		movedPoly = clippedMoved;
		changed = true;
	}
	return changed;
}

bool CoronaAssetExplorer::AlignWorldLayoutAdjacentEdgesForNode(int nodeIndex)
{
	if (nodeIndex < 0 || nodeIndex >= static_cast<int>(LayoutDocument.Nodes.size()))
		return false;
	const std::string movedId = LayoutDocument.Nodes[nodeIndex].Id;
	auto nodePolygon = [](const WorldLayoutNode& node, std::vector<std::pair<float, float>>& out) -> bool
	{
		out.clear();
		if (node.bClosedPolygon && node.Points.size() >= 3)
		{
			out = node.Points;
			return true;
		}
		if (node.bHasSize)
		{
			const float x0 = node.X - node.Width * 0.5f;
			const float x1 = node.X + node.Width * 0.5f;
			const float y0 = node.Y - node.Height2D * 0.5f;
			const float y1 = node.Y + node.Height2D * 0.5f;
			out = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
			return true;
		}
		return false;
	};
	auto setNodePolygon = [&](int i, const std::vector<std::pair<float, float>>& polygon)
	{
		WorldLayoutNode& node = LayoutDocument.Nodes[i];
		node.Points = polygon;
		node.bClosedPolygon = true;
		node.bHasPosition = false;
		node.bHasSize = false;
	};

	struct EdgeMatch
	{
		size_t A = 0;
		size_t B = 0;
		float Tx = 1.0f;
		float Ty = 0.0f;
		float Nx = 0.0f;
		float Ny = 1.0f;
		float SharedC = 0.0f;
		float OverlapMinT = 0.0f;
		float OverlapMaxT = 0.0f;
		float Score = -std::numeric_limits<float>::max();
	};
	auto findBestEdgeMatch = [](const std::vector<std::pair<float, float>>& aPoly,
		const std::vector<std::pair<float, float>>& bPoly,
		EdgeMatch& best) -> bool
	{
		if (aPoly.size() < 3 || bPoly.size() < 3)
			return false;
		const auto aCentroid = LayoutPolygonCentroid(aPoly);
		const auto bCentroid = LayoutPolygonCentroid(bPoly);
		constexpr float kMaxSnapGap = 2400.0f;
		constexpr float kMinOverlap = 120.0f;
		bool found = false;
		for (size_t ai = 0; ai < aPoly.size(); ++ai)
		{
			const auto& a0 = aPoly[ai];
			const auto& a1 = aPoly[(ai + 1) % aPoly.size()];
			float tx = a1.first - a0.first;
			float ty = a1.second - a0.second;
			const float aLen = std::sqrt(tx * tx + ty * ty);
			if (aLen < kMinOverlap)
				continue;
			tx /= aLen;
			ty /= aLen;
			const float nx = -ty;
			const float ny = tx;
			const float aT0 = a0.first * tx + a0.second * ty;
			const float aT1 = a1.first * tx + a1.second * ty;
			const float aMinT = std::min(aT0, aT1);
			const float aMaxT = std::max(aT0, aT1);
			const float aC = (a0.first * nx + a0.second * ny + a1.first * nx + a1.second * ny) * 0.5f;

			for (size_t bi = 0; bi < bPoly.size(); ++bi)
			{
				const auto& b0 = bPoly[bi];
				const auto& b1 = bPoly[(bi + 1) % bPoly.size()];
				float bx = b1.first - b0.first;
				float by = b1.second - b0.second;
				const float bLen = std::sqrt(bx * bx + by * by);
				if (bLen < kMinOverlap)
					continue;
				bx /= bLen;
				by /= bLen;
				const float parallelError = std::abs(tx * by - ty * bx);
				if (parallelError > 0.045f)
					continue;
				const float bC0 = b0.first * nx + b0.second * ny;
				const float bC1 = b1.first * nx + b1.second * ny;
				const float bC = (bC0 + bC1) * 0.5f;
				const float gap = std::abs(bC - aC);
				if (gap > kMaxSnapGap)
					continue;

				const float sharedC = (aC + bC) * 0.5f;
				const float aSide = aCentroid.first * nx + aCentroid.second * ny - sharedC;
				const float bSide = bCentroid.first * nx + bCentroid.second * ny - sharedC;
				if (aSide * bSide >= 0.0f)
					continue;

				float bT0 = b0.first * tx + b0.second * ty;
				float bT1 = b1.first * tx + b1.second * ty;
				if (bT0 > bT1)
					std::swap(bT0, bT1);
				const float overlapMinT = std::max(aMinT, bT0);
				const float overlapMaxT = std::min(aMaxT, bT1);
				const float overlap = overlapMaxT - overlapMinT;
				if (overlap < kMinOverlap)
					continue;

				const float score = overlap - gap * 0.12f - parallelError * 800.0f;
				if (score <= best.Score)
					continue;
				best.A = ai;
				best.B = bi;
				best.Tx = tx;
				best.Ty = ty;
				best.Nx = nx;
				best.Ny = ny;
				best.SharedC = sharedC;
				best.OverlapMinT = overlapMinT;
				best.OverlapMaxT = overlapMaxT;
				best.Score = score;
				found = true;
			}
		}
		return found;
	};

	bool changed = false;
	for (int pass = 0; pass < 8; ++pass)
	{
		nodeIndex = FindWorldLayoutNodeIndex(movedId);
		if (nodeIndex < 0 || nodeIndex >= static_cast<int>(LayoutDocument.Nodes.size()))
			break;
		WorldLayoutNode& movedNode = LayoutDocument.Nodes[nodeIndex];
		if (!IsLayoutPartitionKind(movedNode.Type))
			break;
		std::vector<std::pair<float, float>> movedPoly;
		if (!nodePolygon(movedNode, movedPoly))
			break;

		bool passChanged = false;
		const std::string parent = movedNode.Parent;
		for (int otherIndex = 0; otherIndex < static_cast<int>(LayoutDocument.Nodes.size()); ++otherIndex)
		{
			if (otherIndex == nodeIndex)
				continue;
			const WorldLayoutNode& other = LayoutDocument.Nodes[otherIndex];
			if (other.Parent != parent || !IsLayoutPartitionKind(other.Type))
				continue;
			std::vector<std::pair<float, float>> otherPoly;
			if (!nodePolygon(other, otherPoly))
				continue;

			EdgeMatch match;
			if (!findBestEdgeMatch(movedPoly, otherPoly, match))
				continue;
			std::vector<std::pair<float, float>> snappedMoved = movedPoly;
			std::vector<std::pair<float, float>> snappedOther = otherPoly;
			std::vector<float> sharedTValues;
			AddLayoutTValue(sharedTValues, match.OverlapMinT);
			AddLayoutTValue(sharedTValues, match.OverlapMaxT);
			auto collectEdgeT = [&](const std::vector<std::pair<float, float>>& polygon, size_t edgeIndex)
			{
				const size_t edgeNext = (edgeIndex + 1) % polygon.size();
				const float minT = std::min(match.OverlapMinT, match.OverlapMaxT);
				const float maxT = std::max(match.OverlapMinT, match.OverlapMaxT);
				for (size_t pointIndex : { edgeIndex, edgeNext })
				{
					const auto& p = polygon[pointIndex];
					const float t = p.first * match.Tx + p.second * match.Ty;
					if (t >= minT - 1.0f && t <= maxT + 1.0f)
						AddLayoutTValue(sharedTValues, t);
				}
			};
			collectEdgeT(snappedMoved, match.A);
			collectEdgeT(snappedOther, match.B);
			if (!SnapLayoutPolygonEdgeToSharedLine(snappedMoved, match.A, match.Tx, match.Ty, match.Nx, match.Ny, match.SharedC, match.OverlapMinT, match.OverlapMaxT, sharedTValues))
				continue;
			if (!SnapLayoutPolygonEdgeToSharedLine(snappedOther, match.B, match.Tx, match.Ty, match.Nx, match.Ny, match.SharedC, match.OverlapMinT, match.OverlapMaxT, sharedTValues))
				continue;
			setNodePolygon(nodeIndex, snappedMoved);
			setNodePolygon(otherIndex, snappedOther);
			movedPoly = std::move(snappedMoved);
			changed = true;
			passChanged = true;
			break;
		}
		if (!passChanged)
			break;
	}
	return changed;
}

bool CoronaAssetExplorer::SaveWorldLayoutAsset()
{
	if (!LayoutDocument.bLoaded || LayoutDocument.SourcePath.empty())
		return false;
	std::ofstream file(LayoutDocument.SourcePath, std::ios::trunc);
	if (!file)
	{
		PendingErrorMessage = "save failed: " + LayoutDocument.SourcePath.string();
		return false;
	}

	file << "world name=" << EscapeLayoutValue(LayoutDocument.Name.empty() ? LayoutDocument.SourcePath.stem().string() : LayoutDocument.Name)
		<< " units=centimeters\n\n";
	auto isBuiltInAttr = [](const std::string& key)
	{
		return key == "id" || key == "label" || key == "name" || key == "parent" ||
			key == "pos" || key == "center" || key == "size" || key == "points" ||
			key == "polygon" || key == "footprint" || key == "boundary" ||
			key == "width" || key == "depth" || key == "height" || key == "floors";
	};
	auto isGeneratedOverlapRoad = [](const WorldLayoutNode& node)
	{
		for (const WorldLayoutAttribute& attr : node.Attributes)
		{
			if (attr.Key == "generated" && attr.Value == "overlap_split_road")
				return true;
		}
		return false;
	};
	for (const WorldLayoutNode& node : LayoutDocument.Nodes)
	{
		if (node.Type != "block")
			continue;
		if (isGeneratedOverlapRoad(node))
			continue;
		file << node.Type << " id=" << EscapeLayoutValue(node.Id);
		if (!node.Label.empty() && node.Label != node.Id)
			file << " label=" << EscapeLayoutValue(node.Label);
		if (!node.Parent.empty())
			file << " parent=" << EscapeLayoutValue(node.Parent);
		if (node.bClosedPolygon && node.Points.size() >= 3)
			file << " polygon=" << EscapeLayoutValue(FormatLayoutPointList(node.Points));
		else if (!node.Points.empty())
			file << " points=" << EscapeLayoutValue(FormatLayoutPointList(node.Points));
		else if (node.bHasPosition)
			file << " pos=(" << FormatLayoutFloat(node.X) << "," << FormatLayoutFloat(node.Y) << ")";
		if (node.bHasSize)
			file << " size=(" << FormatLayoutFloat(node.Width) << "," << FormatLayoutFloat(node.Height2D) << ")";
		else if (IsLayoutRoadKind(node.Type) && node.Width > 0.0f)
			file << " width=" << FormatLayoutFloat(node.Width);
		if (node.BuildingHeight > 0.0f)
			file << " height=" << FormatLayoutFloat(node.BuildingHeight);
		if (node.Floors > 0)
			file << " floors=" << node.Floors;
		for (const WorldLayoutAttribute& attr : node.Attributes)
		{
			if (isBuiltInAttr(attr.Key))
				continue;
			file << ' ' << attr.Key << '=' << EscapeLayoutValue(attr.Value);
		}
		file << '\n';
	}
	for (const WorldLayoutEdge& edge : LayoutDocument.Edges)
	{
		if (edge.Type == "contains" || edge.Type == "entrances" || edge.Type == "connects" || edge.Type == "links")
			continue;
		file << "link from=" << EscapeLayoutValue(edge.From)
			<< " to=" << EscapeLayoutValue(edge.To)
			<< " type=" << EscapeLayoutValue(edge.Type.empty() ? "link" : edge.Type)
			<< '\n';
	}
	bWorldLayoutDirty = false;
	PendingErrorMessage.clear();
	AppendCpuRuntimeTrace(L"[AssetExplorer] saved world layout " + LayoutDocument.SourcePath.wstring());
	return true;
}

void CoronaAssetExplorer::CreateProceduralRoadGraph()
{
	RoadDocument = RoadGraphDocument{};
	RoadDocument.Name = "procedural_road_graph";
	RoadDocument.SourcePath = AssetsRoot / L"road_graphs" / L"procedural_road_graph.roadgraph";
	RoadDocument.bLoaded = true;
	GenerateProceduralRoadGraph();
	bRoadGraphViewerVisible = true;
	bRoadGraphAutoFit = true;
	RoadGraphZoom = 1.0f;
	RoadGraphPanX = 0.0f;
	RoadGraphPanY = 0.0f;
	bRoadGraphDirty = true;
	bVisible = false;
}

void CoronaAssetExplorer::GenerateProceduralRoadGraph()
{
	RoadGraphParams params = RoadDocument.Params;
	RoadDocument.Nodes.clear();
	RoadDocument.Edges.clear();
	RoadDocument.Error.clear();
	RoadDocument.bLoaded = true;

	std::mt19937 rng(static_cast<uint32_t>(params.Seed));
	auto rand01 = [&]() -> float
	{
		return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng);
	};
	auto randRange = [&](float a, float b) -> float
	{
		return a + (b - a) * rand01();
	};
	auto clampPoint = [&](std::pair<float, float> p)
	{
		const float hx = params.Width * 0.5f;
		const float hy = params.Height * 0.5f;
		p.first = std::clamp(p.first, -hx, hx);
		p.second = std::clamp(p.second, -hy, hy);
		return p;
	};

	auto addNode = [&](const std::string& kind, float x, float y) -> std::string
	{
		RoadGraphNode node;
		node.Id = "n" + std::to_string(RoadDocument.Nodes.size() + 1);
		node.Kind = kind;
		node.X = x;
		node.Y = y;
		RoadDocument.Nodes.push_back(std::move(node));
		return RoadDocument.Nodes.back().Id;
	};
	auto nodePos = [&](const std::string& id) -> std::pair<float, float>
	{
		for (const RoadGraphNode& node : RoadDocument.Nodes)
		{
			if (node.Id == id)
				return { node.X, node.Y };
		}
		return { 0.0f, 0.0f };
	};
	auto addEdge = [&](const std::string& from, const std::string& to, const std::string& klass, float width, float curveSign)
	{
		const auto a = nodePos(from);
		const auto b = nodePos(to);
		const float dx = b.first - a.first;
		const float dy = b.second - a.second;
		const float len = std::max(1.0f, std::sqrt(dx * dx + dy * dy));
		const float nx = -dy / len;
		const float ny = dx / len;
		const float amp = len * params.Curvature * randRange(0.08f, 0.28f) * curveSign;
		RoadGraphEdge edge;
		edge.Id = "e" + std::to_string(RoadDocument.Edges.size() + 1);
		edge.From = from;
		edge.To = to;
		edge.Class = klass;
		edge.Width = width;
		edge.Points.push_back(a);
		edge.Points.push_back(clampPoint({ (a.first + b.first) * 0.5f + nx * amp, (a.second + b.second) * 0.5f + ny * amp }));
		edge.Points.push_back(b);
		RoadDocument.Edges.push_back(std::move(edge));
	};

	const float hx = params.Width * 0.5f;
	const float hy = params.Height * 0.5f;
	const int arterialCount = std::max(2, params.Arterials);
	const int arterialSegments = std::clamp(5 + static_cast<int>(params.Density * 7.0f), 5, 14);
	std::vector<std::string> arterialNodes;
	for (int a = 0; a < arterialCount; ++a)
	{
		const bool horizontal = (a % 2) == 0;
		const float lane = randRange(-0.78f, 0.78f);
		const float angleJitter = randRange(-0.20f, 0.20f) * params.Curvature;
		const float phase = randRange(0.0f, 6.2831853f);
		const float wave = randRange(1.0f, 2.5f);
		std::vector<std::string> chain;
		for (int s = 0; s <= arterialSegments; ++s)
		{
			const float t = static_cast<float>(s) / arterialSegments;
			float x = horizontal ? -hx + params.Width * t : lane * hx;
			float y = horizontal ? lane * hy : -hy + params.Height * t;
			const float bend = std::sin(t * 6.2831853f * wave + phase) * params.Curvature;
			if (horizontal)
			{
				y += bend * hy * 0.20f;
				x += std::sin(angleJitter) * params.Width * (t - 0.5f);
			}
			else
			{
				x += bend * hx * 0.20f;
				y += std::sin(angleJitter) * params.Height * (t - 0.5f);
			}
			const std::string id = addNode("arterial", clampPoint({ x, y }).first, clampPoint({ x, y }).second);
			chain.push_back(id);
			arterialNodes.push_back(id);
		}
		for (int s = 0; s < arterialSegments; ++s)
			addEdge(chain[s], chain[s + 1], "arterial", 1800.0f, randRange(-1.0f, 1.0f));
	}

	const float branchChance = std::clamp(params.Density * params.Branching * 0.55f, 0.05f, 0.85f);
	const int maxBranchSegments = std::clamp(2 + static_cast<int>(params.Density * 4.0f), 2, 7);
	std::vector<std::string> branchEnds;
	for (const std::string& rootId : arterialNodes)
	{
		if (rand01() > branchChance)
			continue;
		auto root = nodePos(rootId);
		float angle = randRange(0.0f, 6.2831853f);
		const int segments = 1 + static_cast<int>(rand01() * maxBranchSegments);
		std::string prev = rootId;
		for (int s = 0; s < segments; ++s)
		{
			angle += randRange(-0.55f, 0.55f) * params.Curvature;
			const float step = randRange(5200.0f, 11500.0f) * (0.65f + params.Density);
			root.first += std::cos(angle) * step;
			root.second += std::sin(angle) * step;
			root = clampPoint(root);
			const std::string kind = s == 0 ? "collector" : "local";
			const std::string next = addNode(kind, root.first, root.second);
			addEdge(prev, next, kind, s == 0 ? 1100.0f : 720.0f, randRange(-1.0f, 1.0f));
			prev = next;
		}
		branchEnds.push_back(prev);
	}

	const int connectorBudget = std::min(static_cast<int>(branchEnds.size()), static_cast<int>(20 + params.Density * 60.0f));
	for (int i = 0; i < connectorBudget; ++i)
	{
		if (branchEnds.size() < 2 || rand01() > params.Density * 0.45f)
			continue;
		const int a = static_cast<int>(rand01() * branchEnds.size()) % static_cast<int>(branchEnds.size());
		int b = static_cast<int>(rand01() * branchEnds.size()) % static_cast<int>(branchEnds.size());
		if (a == b)
			b = (b + 1) % static_cast<int>(branchEnds.size());
		const auto pa = nodePos(branchEnds[a]);
		const auto pb = nodePos(branchEnds[b]);
		const float dx = pb.first - pa.first;
		const float dy = pb.second - pa.second;
		const float dist = std::sqrt(dx * dx + dy * dy);
		if (dist < 5000.0f || dist > 28000.0f)
			continue;
		addEdge(branchEnds[a], branchEnds[b], "local", 650.0f, randRange(-1.0f, 1.0f));
	}

	PlanarizeRoadGraphIntersections();
	bRoadGraphDirty = true;
}

void CoronaAssetExplorer::PlanarizeRoadGraphIntersections()
{
	if (RoadDocument.Edges.size() < 2)
		return;

	struct RoadCut
	{
		float T = 0.0f;
		std::string NodeId;
		std::pair<float, float> Point;
	};

	constexpr float kSnapDistance = 140.0f;
	constexpr float kCutMergeDistance = 48.0f;
	constexpr float kMinEdgeLength = 80.0f;
	constexpr float kMinCrossingSin = 0.035f;

	auto distanceSq = [](std::pair<float, float> a, std::pair<float, float> b) -> float
	{
		const float dx = a.first - b.first;
		const float dy = a.second - b.second;
		return dx * dx + dy * dy;
	};
	auto distance = [&](std::pair<float, float> a, std::pair<float, float> b) -> float
	{
		return std::sqrt(distanceSq(a, b));
	};
	auto cross = [](float ax, float ay, float bx, float by) -> float
	{
		return ax * by - ay * bx;
	};

	std::unordered_map<std::string, std::pair<float, float>> nodePositions;
	for (const RoadGraphNode& node : RoadDocument.Nodes)
		nodePositions[node.Id] = { node.X, node.Y };

	auto getNodePosition = [&](const std::string& id) -> std::pair<float, float>
	{
		auto it = nodePositions.find(id);
		return it != nodePositions.end() ? it->second : std::pair<float, float>{ 0.0f, 0.0f };
	};

	std::vector<std::vector<std::pair<float, float>>> polylines(RoadDocument.Edges.size());
	std::vector<std::vector<float>> cumulative(RoadDocument.Edges.size());
	std::vector<std::vector<RoadCut>> cuts(RoadDocument.Edges.size());

	auto appendUniquePoint = [&](std::vector<std::pair<float, float>>& points, std::pair<float, float> p)
	{
		if (points.empty() || distanceSq(points.back(), p) > 1.0f)
			points.push_back(p);
	};

	for (size_t edgeIndex = 0; edgeIndex < RoadDocument.Edges.size(); ++edgeIndex)
	{
		const RoadGraphEdge& edge = RoadDocument.Edges[edgeIndex];
		std::vector<std::pair<float, float>>& polyline = polylines[edgeIndex];
		if (edge.Points.size() >= 2)
			polyline = edge.Points;
		else
		{
			polyline.push_back(getNodePosition(edge.From));
			polyline.push_back(getNodePosition(edge.To));
		}

		if (polyline.size() < 2)
			continue;

		const auto fromPos = getNodePosition(edge.From);
		const auto toPos = getNodePosition(edge.To);
		if (distanceSq(polyline.front(), fromPos) > 1.0f)
			polyline.insert(polyline.begin(), fromPos);
		if (distanceSq(polyline.back(), toPos) > 1.0f)
			polyline.push_back(toPos);

		std::vector<float>& cum = cumulative[edgeIndex];
		cum.resize(polyline.size());
		cum[0] = 0.0f;
		for (size_t i = 1; i < polyline.size(); ++i)
			cum[i] = cum[i - 1] + distance(polyline[i - 1], polyline[i]);

		if (cum.back() >= kMinEdgeLength)
		{
			cuts[edgeIndex].push_back({ 0.0f, edge.From, polyline.front() });
			cuts[edgeIndex].push_back({ cum.back(), edge.To, polyline.back() });
		}
	}

	auto findOrCreateJunction = [&](std::pair<float, float> p) -> std::string
	{
		const float snapSq = kSnapDistance * kSnapDistance;
		for (const RoadGraphNode& node : RoadDocument.Nodes)
		{
			if (distanceSq({ node.X, node.Y }, p) <= snapSq)
				return node.Id;
		}

		RoadGraphNode node;
		node.Id = "n" + std::to_string(RoadDocument.Nodes.size() + 1);
		node.Kind = "junction";
		node.X = p.first;
		node.Y = p.second;
		RoadDocument.Nodes.push_back(node);
		nodePositions[node.Id] = p;
		return node.Id;
	};

	auto segmentIntersection = [&](std::pair<float, float> a, std::pair<float, float> b,
		std::pair<float, float> c, std::pair<float, float> d,
		float& outUa, float& outUb, std::pair<float, float>& outPoint) -> bool
	{
		const float rx = b.first - a.first;
		const float ry = b.second - a.second;
		const float sx = d.first - c.first;
		const float sy = d.second - c.second;
		const float rLen = std::sqrt(rx * rx + ry * ry);
		const float sLen = std::sqrt(sx * sx + sy * sy);
		if (rLen < kMinEdgeLength || sLen < kMinEdgeLength)
			return false;

		const float denom = cross(rx, ry, sx, sy);
		const float crossingSin = std::abs(denom) / std::max(1.0f, rLen * sLen);
		if (crossingSin < kMinCrossingSin)
			return false;

		const float qpx = c.first - a.first;
		const float qpy = c.second - a.second;
		outUa = cross(qpx, qpy, sx, sy) / denom;
		outUb = cross(qpx, qpy, rx, ry) / denom;
		if (outUa <= 0.001f || outUa >= 0.999f || outUb <= 0.001f || outUb >= 0.999f)
			return false;

		outPoint = { a.first + rx * outUa, a.second + ry * outUa };
		return true;
	};

	for (size_t a = 0; a < RoadDocument.Edges.size(); ++a)
	{
		const RoadGraphEdge& edgeA = RoadDocument.Edges[a];
		if (polylines[a].size() < 2 || cumulative[a].empty())
			continue;
		for (size_t b = a + 1; b < RoadDocument.Edges.size(); ++b)
		{
			const RoadGraphEdge& edgeB = RoadDocument.Edges[b];
			if (edgeA.From == edgeB.From || edgeA.From == edgeB.To || edgeA.To == edgeB.From || edgeA.To == edgeB.To)
				continue;
			if (polylines[b].size() < 2 || cumulative[b].empty())
				continue;

			for (size_t sa = 0; sa + 1 < polylines[a].size(); ++sa)
			{
				for (size_t sb = 0; sb + 1 < polylines[b].size(); ++sb)
				{
					float ua = 0.0f;
					float ub = 0.0f;
					std::pair<float, float> hit;
					if (!segmentIntersection(polylines[a][sa], polylines[a][sa + 1], polylines[b][sb], polylines[b][sb + 1], ua, ub, hit))
						continue;

					const float ta = cumulative[a][sa] + (cumulative[a][sa + 1] - cumulative[a][sa]) * ua;
					const float tb = cumulative[b][sb] + (cumulative[b][sb + 1] - cumulative[b][sb]) * ub;
					if (ta < kMinEdgeLength || cumulative[a].back() - ta < kMinEdgeLength ||
						tb < kMinEdgeLength || cumulative[b].back() - tb < kMinEdgeLength)
						continue;

					const std::string nodeId = findOrCreateJunction(hit);
					cuts[a].push_back({ ta, nodeId, hit });
					cuts[b].push_back({ tb, nodeId, hit });
				}
			}
		}
	}

	auto sampleAt = [&](const std::vector<std::pair<float, float>>& polyline, const std::vector<float>& cum, float t)
	{
		if (polyline.empty())
			return std::pair<float, float>{ 0.0f, 0.0f };
		if (t <= 0.0f)
			return polyline.front();
		if (t >= cum.back())
			return polyline.back();
		for (size_t i = 1; i < cum.size(); ++i)
		{
			if (t > cum[i])
				continue;
			const float span = std::max(1.0f, cum[i] - cum[i - 1]);
			const float alpha = std::clamp((t - cum[i - 1]) / span, 0.0f, 1.0f);
			return std::pair<float, float>{
				polyline[i - 1].first + (polyline[i].first - polyline[i - 1].first) * alpha,
				polyline[i - 1].second + (polyline[i].second - polyline[i - 1].second) * alpha
			};
		}
		return polyline.back();
	};

	std::vector<RoadGraphEdge> rebuiltEdges;
	int nextEdgeId = 1;
	for (size_t edgeIndex = 0; edgeIndex < RoadDocument.Edges.size(); ++edgeIndex)
	{
		const RoadGraphEdge& sourceEdge = RoadDocument.Edges[edgeIndex];
		if (polylines[edgeIndex].size() < 2 || cumulative[edgeIndex].empty() || cuts[edgeIndex].size() < 2)
		{
			rebuiltEdges.push_back(sourceEdge);
			rebuiltEdges.back().Id = "e" + std::to_string(nextEdgeId++);
			continue;
		}

		std::vector<RoadCut>& edgeCuts = cuts[edgeIndex];
		std::sort(edgeCuts.begin(), edgeCuts.end(), [](const RoadCut& a, const RoadCut& b)
		{
			return a.T < b.T;
		});

		std::vector<RoadCut> uniqueCuts;
		for (const RoadCut& cut : edgeCuts)
		{
			if (!uniqueCuts.empty() &&
				(std::abs(cut.T - uniqueCuts.back().T) < kCutMergeDistance || distanceSq(cut.Point, uniqueCuts.back().Point) < kCutMergeDistance * kCutMergeDistance))
				continue;
			uniqueCuts.push_back(cut);
		}

		for (size_t cutIndex = 0; cutIndex + 1 < uniqueCuts.size(); ++cutIndex)
		{
			const RoadCut& a = uniqueCuts[cutIndex];
			const RoadCut& b = uniqueCuts[cutIndex + 1];
			if (a.NodeId == b.NodeId || b.T - a.T < kMinEdgeLength)
				continue;

			RoadGraphEdge edge;
			edge.Id = "e" + std::to_string(nextEdgeId++);
			edge.From = a.NodeId;
			edge.To = b.NodeId;
			edge.Class = sourceEdge.Class;
			edge.Width = sourceEdge.Width;
			appendUniquePoint(edge.Points, sampleAt(polylines[edgeIndex], cumulative[edgeIndex], a.T));
			for (size_t p = 1; p + 1 < polylines[edgeIndex].size(); ++p)
			{
				if (cumulative[edgeIndex][p] > a.T + kCutMergeDistance && cumulative[edgeIndex][p] < b.T - kCutMergeDistance)
					appendUniquePoint(edge.Points, polylines[edgeIndex][p]);
			}
			appendUniquePoint(edge.Points, sampleAt(polylines[edgeIndex], cumulative[edgeIndex], b.T));
			rebuiltEdges.push_back(std::move(edge));
		}
	}

	RoadDocument.Edges = std::move(rebuiltEdges);
}

void CoronaAssetExplorer::OpenRoadGraphAsset(const std::filesystem::path& path)
{
	RoadGraphDocument loaded;
	loaded.SourcePath = path;
	loaded.Name = path.stem().string();
	std::ifstream file(path);
	if (!file)
	{
		PendingErrorMessage = "open failed: " + path.string();
		loaded.Error = PendingErrorMessage;
		RoadDocument = std::move(loaded);
		bRoadGraphViewerVisible = true;
		return;
	}

	std::string line;
	int lineNumber = 0;
	while (std::getline(file, line))
	{
		++lineNumber;
		line = Trim(StripComment(line));
		if (line.empty())
			continue;
		std::vector<std::string> tokens = TokenizeLayoutLine(line);
		if (tokens.empty())
			continue;
		std::string kind = tokens[0];
		std::transform(kind.begin(), kind.end(), kind.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		const auto values = ParseKeyValues(tokens, 1);
		if (kind == "roadgraph")
		{
			auto it = values.find("name");
			if (it != values.end() && !it->second.empty())
				loaded.Name = it->second;
			continue;
		}
		if (kind == "params")
		{
			loaded.Params.Seed = ParseIntOr(values, "seed", loaded.Params.Seed);
			loaded.Params.Width = ParseFloatOr(values, "width", loaded.Params.Width);
			loaded.Params.Height = ParseFloatOr(values, "height", loaded.Params.Height);
			loaded.Params.Density = ParseFloatOr(values, "density", loaded.Params.Density);
			loaded.Params.Curvature = ParseFloatOr(values, "curvature", loaded.Params.Curvature);
			loaded.Params.Branching = ParseFloatOr(values, "branching", loaded.Params.Branching);
			loaded.Params.Arterials = ParseIntOr(values, "arterials", loaded.Params.Arterials);
			continue;
		}
		if (kind == "node")
		{
			RoadGraphNode node;
			auto idIt = values.find("id");
			node.Id = (idIt != values.end() && !idIt->second.empty()) ? idIt->second : ("n" + std::to_string(loaded.Nodes.size() + 1));
			auto kindIt = values.find("kind");
			node.Kind = (kindIt != values.end()) ? kindIt->second : "road";
			auto posIt = values.find("pos");
			if (posIt == values.end() || !ParseFloatPair(posIt->second, node.X, node.Y))
			{
				loaded.Error += "line " + std::to_string(lineNumber) + ": node requires pos=(x,y)\n";
				continue;
			}
			loaded.Nodes.push_back(std::move(node));
			continue;
		}
		if (kind == "edge")
		{
			RoadGraphEdge edge;
			auto idIt = values.find("id");
			edge.Id = (idIt != values.end() && !idIt->second.empty()) ? idIt->second : ("e" + std::to_string(loaded.Edges.size() + 1));
			auto fromIt = values.find("from");
			auto toIt = values.find("to");
			edge.From = fromIt != values.end() ? fromIt->second : std::string();
			edge.To = toIt != values.end() ? toIt->second : std::string();
			auto classIt = values.find("class");
			edge.Class = classIt != values.end() ? classIt->second : "local";
			edge.Width = ParseFloatOr(values, "width", edge.Width);
			auto pointsIt = values.find("points");
			if (pointsIt != values.end())
				ParsePointList(pointsIt->second, edge.Points);
			if (edge.From.empty() || edge.To.empty())
				loaded.Error += "line " + std::to_string(lineNumber) + ": edge requires from/to\n";
			else
				loaded.Edges.push_back(std::move(edge));
			continue;
		}
		loaded.Error += "line " + std::to_string(lineNumber) + ": unknown roadgraph token '" + kind + "'\n";
	}

	loaded.bLoaded = true;
	RoadDocument = std::move(loaded);
	bRoadGraphViewerVisible = true;
	bRoadGraphAutoFit = true;
	RoadGraphZoom = 1.0f;
	RoadGraphPanX = 0.0f;
	RoadGraphPanY = 0.0f;
	bRoadGraphDirty = false;
	PendingErrorMessage.clear();
	bVisible = false;
	AppendCpuRuntimeTrace(L"[AssetExplorer] opened road graph " + path.wstring());
}

bool CoronaAssetExplorer::SaveRoadGraphAsset()
{
	if (!RoadDocument.bLoaded)
		return false;
	if (RoadDocument.SourcePath.empty())
		RoadDocument.SourcePath = AssetsRoot / L"road_graphs" / L"procedural_road_graph.roadgraph";
	std::error_code ec;
	std::filesystem::create_directories(RoadDocument.SourcePath.parent_path(), ec);
	std::ofstream file(RoadDocument.SourcePath, std::ios::trunc);
	if (!file)
	{
		PendingErrorMessage = "save failed: " + RoadDocument.SourcePath.string();
		return false;
	}
	const RoadGraphParams& p = RoadDocument.Params;
	file << "roadgraph name=" << EscapeLayoutValue(RoadDocument.Name.empty() ? RoadDocument.SourcePath.stem().string() : RoadDocument.Name) << "\n";
	file << "params seed=" << p.Seed
		<< " width=" << FormatLayoutFloat(p.Width)
		<< " height=" << FormatLayoutFloat(p.Height)
		<< " density=" << FormatLayoutFloat(p.Density)
		<< " curvature=" << FormatLayoutFloat(p.Curvature)
		<< " branching=" << FormatLayoutFloat(p.Branching)
		<< " arterials=" << p.Arterials << "\n\n";
	for (const RoadGraphNode& node : RoadDocument.Nodes)
		file << "node id=" << EscapeLayoutValue(node.Id) << " kind=" << EscapeLayoutValue(node.Kind)
			<< " pos=(" << FormatLayoutFloat(node.X) << "," << FormatLayoutFloat(node.Y) << ")\n";
	file << "\n";
	for (const RoadGraphEdge& edge : RoadDocument.Edges)
		file << "edge id=" << EscapeLayoutValue(edge.Id)
			<< " from=" << EscapeLayoutValue(edge.From)
			<< " to=" << EscapeLayoutValue(edge.To)
			<< " class=" << EscapeLayoutValue(edge.Class)
			<< " width=" << FormatLayoutFloat(edge.Width)
			<< " points=" << EscapeLayoutValue(FormatLayoutPointList(edge.Points)) << "\n";
	bRoadGraphDirty = false;
	PendingErrorMessage.clear();
	AppendCpuRuntimeTrace(L"[AssetExplorer] saved road graph " + RoadDocument.SourcePath.wstring());
	return true;
}

void CoronaAssetExplorer::OpenCityLayoutAsset(const std::filesystem::path& path)
{
	CityLayoutDocument loaded;
	loaded.SourcePath = path;
	loaded.Name = path.stem().string();

	std::ifstream file(path);
	if (!file)
	{
		PendingErrorMessage = "open failed: " + path.string();
		loaded.Error = PendingErrorMessage;
		CityDocument = std::move(loaded);
		bCityLayoutViewerVisible = true;
		return;
	}

	try
	{
		nlohmann::json root;
		file >> root;
		loaded.Name = root.value("name", loaded.Name);
		const nlohmann::json params = root.contains("params") ? root["params"] :
			(root.contains("generation") ? root["generation"] : nlohmann::json::object());
		loaded.Params.Seed = params.value("seed", loaded.Params.Seed);
		loaded.Params.Width = params.value("width", loaded.Params.Width);
		loaded.Params.Depth = params.value("depth", loaded.Params.Depth);
		loaded.Params.RoadSpacing = params.value("road_spacing", loaded.Params.RoadSpacing);
		loaded.Params.RoadJitter = params.value("road_jitter", loaded.Params.RoadJitter);
		loaded.Params.RoadWidth = params.value("road_width", loaded.Params.RoadWidth);
		loaded.Params.BuildingDensity = params.value("building_density", loaded.Params.BuildingDensity);
		loaded.Params.BuildingSizeVariance = params.value("building_size_variance", loaded.Params.BuildingSizeVariance);
		loaded.Params.LotSize = params.value("lot_size", loaded.Params.LotSize);
		loaded.Params.Setback = params.value("setback", loaded.Params.Setback);
		loaded.Params.MinBuildingHeight = params.value("min_building_height", loaded.Params.MinBuildingHeight);
		loaded.Params.MaxBuildingHeight = params.value("max_building_height", loaded.Params.MaxBuildingHeight);
		if (root.contains("bounds") && root["bounds"].is_array() && root["bounds"].size() >= 4)
		{
			const float minX = root["bounds"][0].get<float>();
			const float minY = root["bounds"][1].get<float>();
			const float maxX = root["bounds"][2].get<float>();
			const float maxY = root["bounds"][3].get<float>();
			loaded.Params.Width = std::max(1000.0f, std::abs(maxX - minX));
			loaded.Params.Depth = std::max(1000.0f, std::abs(maxY - minY));
		}

		if (root.contains("building_types") && root["building_types"].is_array())
		{
			for (const nlohmann::json& typeJson : root["building_types"])
			{
				CityLayoutBuildingType type;
				type.Id = typeJson.value("id", std::string("building"));
				type.Weight = typeJson.value("weight", type.Weight);
				type.MinHeight = typeJson.value("min_height", type.MinHeight);
				type.MaxHeight = typeJson.value("max_height", type.MaxHeight);
				type.bBrickTexture = typeJson.value("brick_texture", type.bBrickTexture);
				if (typeJson.contains("color") && typeJson["color"].is_array() && typeJson["color"].size() >= 3)
				{
					type.Color.R = typeJson["color"][0].get<float>();
					type.Color.G = typeJson["color"][1].get<float>();
					type.Color.B = typeJson["color"][2].get<float>();
				}
				loaded.BuildingTypes.push_back(type);
			}
		}

		if (root.contains("roads") && root["roads"].is_array())
		{
			for (const nlohmann::json& roadJson : root["roads"])
			{
				CityLayoutRoad road;
				road.Id = roadJson.value("id", "road_" + std::to_string(loaded.Roads.size() + 1));
				road.Class = roadJson.value("class", std::string("local"));
				road.Width = roadJson.value("width", loaded.Params.RoadWidth);
				road.bLocked = roadJson.value("locked", false);
				const nlohmann::json vertices = roadJson.contains("vertices") ? roadJson["vertices"] :
					(roadJson.contains("points") ? roadJson["points"] : nlohmann::json::array());
				if (vertices.is_array())
				{
					for (const nlohmann::json& pointJson : vertices)
					{
						if (pointJson.is_array() && pointJson.size() >= 2)
							road.Points.emplace_back(pointJson[0].get<float>(), pointJson[1].get<float>());
					}
				}
				if (road.Points.size() >= 2)
					loaded.Roads.push_back(std::move(road));
			}
		}
	}
	catch (const std::exception& e)
	{
		loaded.Error = e.what();
		PendingErrorMessage = "city layout parse failed: " + loaded.Error;
	}

	loaded.bLoaded = true;
	CityDocument = std::move(loaded);
	if (CityDocument.BuildingTypes.empty())
	{
		CityDocument.BuildingTypes = {
			{ "residential", 1.0f, 180.0f, 720.0f, { 0.62f, 0.66f, 0.70f }, true },
			{ "commercial", 0.65f, 320.0f, 1200.0f, { 0.52f, 0.58f, 0.66f }, false },
			{ "industrial", 0.35f, 160.0f, 520.0f, { 0.50f, 0.48f, 0.42f }, true }
		};
	}
	if (CityDocument.Roads.empty())
		GenerateCityLayoutRoads();
	else
		RebuildCityLayoutBuildings();

	bCityLayoutViewerVisible = true;
	bCityLayoutAutoFit = true;
	CityLayoutZoom = 1.0f;
	CityLayoutPanX = 0.0f;
	CityLayoutPanY = 0.0f;
	bCityLayoutDirty = false;
	PendingErrorMessage.clear();
	bVisible = false;
	AppendCpuRuntimeTrace(L"[AssetExplorer] opened city layout " + path.wstring());
}

void CoronaAssetExplorer::GenerateCityLayoutRoads()
{
	if (!CityDocument.bLoaded)
		return;

	const CityLayoutParams& p = CityDocument.Params;
	CityDocument.Roads.clear();
	std::mt19937 rng(static_cast<uint32_t>(p.Seed));
	auto rand01 = [&]() -> float { return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng); };
	auto randRange = [&](float a, float b) -> float { return a + (b - a) * rand01(); };

	const float hx = std::max(500.0f, p.Width * 0.5f);
	const float hy = std::max(500.0f, p.Depth * 0.5f);
	const float spacing = std::max(500.0f, p.RoadSpacing);
	std::vector<float> xs;
	std::vector<float> ys;
	for (float x = -hx; x <= hx + 1.0f; x += spacing)
		xs.push_back(std::clamp(x, -hx, hx));
	for (float y = -hy; y <= hy + 1.0f; y += spacing)
		ys.push_back(std::clamp(y, -hy, hy));
	if (xs.empty() || std::abs(xs.back() - hx) > 1.0f)
		xs.push_back(hx);
	if (ys.empty() || std::abs(ys.back() - hy) > 1.0f)
		ys.push_back(hy);

	std::vector<std::vector<std::pair<float, float>>> intersections(
		xs.size(),
		std::vector<std::pair<float, float>>(ys.size()));
	for (size_t ix = 0; ix < xs.size(); ++ix)
	{
		for (size_t iy = 0; iy < ys.size(); ++iy)
		{
			const bool borderX = ix == 0 || ix + 1 == xs.size();
			const bool borderY = iy == 0 || iy + 1 == ys.size();
			const float jitterX = (borderX || borderY) ? 0.0f : randRange(-p.RoadJitter, p.RoadJitter);
			const float jitterY = (borderX || borderY) ? 0.0f : randRange(-p.RoadJitter, p.RoadJitter);
			intersections[ix][iy] = {
				std::clamp(xs[ix] + jitterX, -hx, hx),
				std::clamp(ys[iy] + jitterY, -hy, hy)
			};
		}
	}

	auto addRoad = [&](const std::string& klass, float width, std::vector<std::pair<float, float>> points)
	{
		CityLayoutRoad road;
		road.Id = "r" + std::to_string(CityDocument.Roads.size() + 1);
		road.Class = klass;
		road.Width = width;
		road.Points = std::move(points);
		CityDocument.Roads.push_back(std::move(road));
	};

	for (size_t ix = 0; ix < xs.size(); ++ix)
	{
		std::vector<std::pair<float, float>> points;
		const bool arterial = ix == 0 || ix + 1 == xs.size() || ix == xs.size() / 2;
		for (size_t iy = 0; iy < ys.size(); ++iy)
			points.push_back(intersections[ix][iy]);
		addRoad(arterial ? "arterial" : "local", arterial ? p.RoadWidth * 1.65f : p.RoadWidth, std::move(points));
	}
	for (size_t iy = 0; iy < ys.size(); ++iy)
	{
		std::vector<std::pair<float, float>> points;
		const bool arterial = iy == 0 || iy + 1 == ys.size() || iy == ys.size() / 2;
		for (size_t ix = 0; ix < xs.size(); ++ix)
			points.push_back(intersections[ix][iy]);
		addRoad(arterial ? "arterial" : "local", arterial ? p.RoadWidth * 1.65f : p.RoadWidth, std::move(points));
	}

	SelectedCityRoad = -1;
	DraggingCityRoad = -1;
	DraggingCityVertex = -1;
	RebuildCityLayoutBuildings();
	bCityLayoutDirty = true;
}

void CoronaAssetExplorer::RebuildCityLayoutBuildings()
{
	if (!CityDocument.bLoaded)
		return;
	if (CityDocument.BuildingTypes.empty())
	{
		CityDocument.BuildingTypes = {
			{ "residential", 1.0f, 180.0f, 720.0f, { 0.62f, 0.66f, 0.70f }, true },
			{ "commercial", 0.65f, 320.0f, 1200.0f, { 0.52f, 0.58f, 0.66f }, false },
			{ "industrial", 0.35f, 160.0f, 520.0f, { 0.50f, 0.48f, 0.42f }, true }
		};
	}

	const CityLayoutParams& p = CityDocument.Params;
	CityDocument.Buildings.clear();
	std::mt19937 rng(static_cast<uint32_t>(p.Seed) ^ 0x9E3779B9u);
	auto rand01 = [&]() -> float { return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng); };
	auto randRange = [&](float a, float b) -> float { return a + (b - a) * rand01(); };
	auto distToSegment = [](std::pair<float, float> p, std::pair<float, float> a, std::pair<float, float> b) -> float
	{
		const float vx = b.first - a.first;
		const float vy = b.second - a.second;
		const float wx = p.first - a.first;
		const float wy = p.second - a.second;
		const float lenSq = vx * vx + vy * vy;
		const float t = lenSq > 1.0e-4f ? std::clamp((wx * vx + wy * vy) / lenSq, 0.0f, 1.0f) : 0.0f;
		const float dx = p.first - (a.first + vx * t);
		const float dy = p.second - (a.second + vy * t);
		return std::sqrt(dx * dx + dy * dy);
	};
	auto roadClearance = [&](std::pair<float, float> point) -> float
	{
		float best = std::numeric_limits<float>::max();
		for (const CityLayoutRoad& road : CityDocument.Roads)
		{
			for (size_t i = 1; i < road.Points.size(); ++i)
				best = std::min(best, distToSegment(point, road.Points[i - 1], road.Points[i]) - road.Width * 0.5f);
		}
		return best;
	};
	auto chooseType = [&]() -> const CityLayoutBuildingType&
	{
		float total = 0.0f;
		for (const CityLayoutBuildingType& type : CityDocument.BuildingTypes)
			total += std::max(type.Weight, 0.0f);
		float r = randRange(0.0f, std::max(total, 0.001f));
		for (const CityLayoutBuildingType& type : CityDocument.BuildingTypes)
		{
			r -= std::max(type.Weight, 0.0f);
			if (r <= 0.0f)
				return type;
		}
		return CityDocument.BuildingTypes.back();
	};

	const float hx = std::max(500.0f, p.Width * 0.5f);
	const float hy = std::max(500.0f, p.Depth * 0.5f);
	const float lot = std::max(240.0f, p.LotSize);
	const float sizeVariance = std::clamp(p.BuildingSizeVariance, 0.0f, 2.0f);
	const float buildingSizeMid = 0.64f;
	const float buildingSizeHalfRange = 0.22f * sizeVariance;
	const float buildingSizeMin = std::clamp(buildingSizeMid - buildingSizeHalfRange, 0.16f, 0.98f);
	const float buildingSizeMax = std::clamp(buildingSizeMid + buildingSizeHalfRange, buildingSizeMin, 0.98f);
	int buildingIndex = 1;
	for (float y = -hy + lot * 0.5f; y <= hy - lot * 0.5f; y += lot)
	{
		for (float x = -hx + lot * 0.5f; x <= hx - lot * 0.5f; x += lot)
		{
			if (rand01() > std::clamp(p.BuildingDensity, 0.0f, 1.0f))
				continue;
			const float bw = lot * randRange(buildingSizeMin, buildingSizeMax);
			const float bd = lot * randRange(buildingSizeMin, buildingSizeMax);
			const std::pair<float, float> center = {
				x + randRange(-lot * 0.16f, lot * 0.16f),
				y + randRange(-lot * 0.16f, lot * 0.16f)
			};
			const float radius = 0.5f * std::sqrt(bw * bw + bd * bd);
			if (roadClearance(center) < p.Setback + radius)
				continue;

			const CityLayoutBuildingType& type = chooseType();
			CityLayoutBuilding b;
			b.Id = "b" + std::to_string(buildingIndex++);
			b.Type = type.Id;
			b.X = center.first;
			b.Y = center.second;
			b.Width = bw;
			b.Depth = bd;
			const float minH = std::max(p.MinBuildingHeight, type.MinHeight);
			const float maxH = std::max(minH + 10.0f, std::min(p.MaxBuildingHeight, type.MaxHeight));
			b.Height = randRange(minH, maxH);
			b.Rotation = rand01() > 0.5f ? 0.0f : 90.0f;
			b.Color = type.Color;
			b.bBrickTexture = type.bBrickTexture;
			CityDocument.Buildings.push_back(b);
		}
	}
}

bool CoronaAssetExplorer::SaveCityLayoutAsset()
{
	if (!CityDocument.bLoaded)
		return false;
	if (CityDocument.SourcePath.empty())
		CityDocument.SourcePath = AssetsRoot / L"city_layouts" / L"procedural_city.citylayout";
	std::error_code ec;
	std::filesystem::create_directories(CityDocument.SourcePath.parent_path(), ec);

	nlohmann::json root;
	root["format"] = "corona_city_layout.v1";
	root["name"] = CityDocument.Name.empty() ? CityDocument.SourcePath.stem().string() : CityDocument.Name;
	root["params"] = {
		{ "seed", CityDocument.Params.Seed },
		{ "width", CityDocument.Params.Width },
		{ "depth", CityDocument.Params.Depth },
		{ "road_spacing", CityDocument.Params.RoadSpacing },
		{ "road_jitter", CityDocument.Params.RoadJitter },
		{ "road_width", CityDocument.Params.RoadWidth },
		{ "building_density", CityDocument.Params.BuildingDensity },
		{ "building_size_variance", CityDocument.Params.BuildingSizeVariance },
		{ "lot_size", CityDocument.Params.LotSize },
		{ "setback", CityDocument.Params.Setback },
		{ "min_building_height", CityDocument.Params.MinBuildingHeight },
		{ "max_building_height", CityDocument.Params.MaxBuildingHeight }
	};
	root["building_types"] = nlohmann::json::array();
	for (const CityLayoutBuildingType& type : CityDocument.BuildingTypes)
	{
		root["building_types"].push_back({
			{ "id", type.Id },
			{ "weight", type.Weight },
			{ "min_height", type.MinHeight },
			{ "max_height", type.MaxHeight },
			{ "color", { type.Color.R, type.Color.G, type.Color.B } },
			{ "brick_texture", type.bBrickTexture }
		});
	}
	root["roads"] = nlohmann::json::array();
	for (const CityLayoutRoad& road : CityDocument.Roads)
	{
		nlohmann::json roadJson;
		roadJson["id"] = road.Id;
		roadJson["class"] = road.Class;
		roadJson["width"] = road.Width;
		roadJson["locked"] = road.bLocked;
		roadJson["vertices"] = nlohmann::json::array();
		for (const auto& point : road.Points)
			roadJson["vertices"].push_back({ point.first, point.second });
		root["roads"].push_back(std::move(roadJson));
	}

	std::ofstream file(CityDocument.SourcePath, std::ios::trunc);
	if (!file)
	{
		PendingErrorMessage = "save failed: " + CityDocument.SourcePath.string();
		return false;
	}
	file << root.dump(2) << "\n";
	bCityLayoutDirty = false;
	PendingErrorMessage.clear();
	AppendCpuRuntimeTrace(L"[AssetExplorer] saved city layout " + CityDocument.SourcePath.wstring());
	return true;
}

bool CoronaAssetExplorer::CompileCityLayoutToMapAndLoad()
{
	if (!CityDocument.bLoaded)
		return false;
	const std::string mapName = (CityDocument.Name.empty() ? std::string("procedural_city") : CityDocument.Name) + "_compiled";
	const std::filesystem::path mapPath = AssetsRoot / L"maps" / (PlatformUtf8ToWide(mapName) + L".map");
	std::error_code ec;
	std::filesystem::create_directories(mapPath.parent_path(), ec);
	std::ofstream file(mapPath, std::ios::trunc);
	if (!file)
	{
		PendingErrorMessage = "compile failed: " + mapPath.string();
		return false;
	}

	auto writeBox = [&](const std::string& name, CityLayoutColor color, bool brick, float uvRepeat,
		float x, float y, float z, float rx, float ry, float rz, float sx, float sy, float sz, float roughness,
		const std::string& textureKind = std::string(), float uvRepeatY = -1.0f,
		const std::string& primitive = std::string("BOX"), bool frontOnly = false)
	{
		file << "    {\n";
		file << "      name = " << EscapeLuaStringForMap(name) << ",\n";
		file << "      mesh = {\n";
		file << "        primitive = " << EscapeLuaStringForMap(primitive) << ",\n";
		file << "        color = {" << FormatLayoutFloat(color.R) << "," << FormatLayoutFloat(color.G) << "," << FormatLayoutFloat(color.B) << "},\n";
		file << "        brick_texture = " << (brick ? "true" : "false") << ",\n";
		if (frontOnly)
			file << "        front_only = true,\n";
		file << "        uv_repeat = " << FormatLayoutFloat(uvRepeat) << ",\n";
		if (uvRepeatY > 0.0f)
			file << "        uv_repeat_y = " << FormatLayoutFloat(uvRepeatY) << ",\n";
		if (!textureKind.empty())
			file << "        texture_kind = " << EscapeLuaStringForMap(textureKind) << ",\n";
		file << "        transform = {\n";
		file << "          position = {" << FormatLayoutFloat(x) << "," << FormatLayoutFloat(y) << "," << FormatLayoutFloat(z) << "},\n";
		file << "          rotation = {" << FormatLayoutFloat(rx) << "," << FormatLayoutFloat(ry) << "," << FormatLayoutFloat(rz) << "},\n";
		file << "          scale = {" << FormatLayoutFloat(sx) << "," << FormatLayoutFloat(sy) << "," << FormatLayoutFloat(sz) << "},\n";
		file << "        },\n";
		file << "        material = { roughness = " << FormatLayoutFloat(roughness) << ", metallic = 0, override = true },\n";
		file << "        ray_tracing = true,\n";
		file << "        visible = true,\n";
		file << "      },\n";
		file << "    },\n";
	};
	auto writeRoadDecal = [&](const std::string& name, CityLayoutColor color,
		float x, float y, float z, float ry, float length, float width, float roughness, float uvRepeat, float uvRepeatY)
	{
		file << "    {\n";
		file << "      name = " << EscapeLuaStringForMap(name) << ",\n";
		file << "      decal = {\n";
		file << "        type = \"road\",\n";
		file << "        position = {" << FormatLayoutFloat(x) << "," << FormatLayoutFloat(y) << "," << FormatLayoutFloat(z) << "},\n";
		file << "        rotation = {0," << FormatLayoutFloat(ry) << ",0},\n";
		file << "        size = {" << FormatLayoutFloat(length) << "," << FormatLayoutFloat(width) << "},\n";
		file << "        color = {" << FormatLayoutFloat(color.R) << "," << FormatLayoutFloat(color.G) << "," << FormatLayoutFloat(color.B) << ",1},\n";
		file << "        roughness = " << FormatLayoutFloat(roughness) << ",\n";
		file << "        uv_repeat = " << FormatLayoutFloat(uvRepeat) << ",\n";
		file << "        uv_repeat_y = " << FormatLayoutFloat(uvRepeatY) << ",\n";
		file << "        height_tolerance = 24,\n";
		file << "        normal_threshold = 0.65,\n";
		file << "        enabled = true,\n";
		file << "      },\n";
		file << "    },\n";
	};
	auto writeRoadPolygonDecal = [&](const std::string& name, CityLayoutColor color,
		float x, float y, float z, float ry, float length, float width,
		const std::vector<glm::vec2>& polygon, float roughness, float uvRepeat, float uvRepeatY, bool centerStripe)
	{
		if (polygon.size() < 3)
			return;
		file << "    {\n";
		file << "      name = " << EscapeLuaStringForMap(name) << ",\n";
		file << "      decal = {\n";
		file << "        type = \"road\",\n";
		file << "        position = {" << FormatLayoutFloat(x) << "," << FormatLayoutFloat(y) << "," << FormatLayoutFloat(z) << "},\n";
		file << "        rotation = {0," << FormatLayoutFloat(ry) << ",0},\n";
		file << "        size = {" << FormatLayoutFloat(length) << "," << FormatLayoutFloat(width) << "},\n";
		file << "        polygon = {\n";
		for (const glm::vec2& p : polygon)
			file << "          {" << FormatLayoutFloat(p.x) << "," << FormatLayoutFloat(p.y) << "},\n";
		file << "        },\n";
		file << "        color = {" << FormatLayoutFloat(color.R) << "," << FormatLayoutFloat(color.G) << "," << FormatLayoutFloat(color.B) << ",1},\n";
		file << "        roughness = " << FormatLayoutFloat(roughness) << ",\n";
		file << "        uv_repeat = " << FormatLayoutFloat(uvRepeat) << ",\n";
		file << "        uv_repeat_y = " << FormatLayoutFloat(uvRepeatY) << ",\n";
		file << "        height_tolerance = 24,\n";
		file << "        normal_threshold = 0.65,\n";
		file << "        center_stripe = " << (centerStripe ? "true" : "false") << ",\n";
		file << "        enabled = true,\n";
		file << "      },\n";
		file << "    },\n";
	};

	file << "-- Corona map compiled from editable city layout\n";
	file << "return {\n";
	file << "  version = 1,\n";
	file << "  name = " << EscapeLuaStringForMap(mapName) << ",\n";
	file << "  entities = {\n";

	writeBox("City_Ground", { 0.22f, 0.24f, 0.23f }, false, 18.0f,
		0.0f, -18.0f, 0.0f, 0.0f, 0.0f, 0.0f,
		CityDocument.Params.Width + 1400.0f, 24.0f, CityDocument.Params.Depth + 1400.0f, 0.92f);

	struct RawRoadSegment
	{
		glm::vec2 A = glm::vec2(0.0f);
		glm::vec2 B = glm::vec2(0.0f);
		float Width = 24.0f;
		CityLayoutColor Color;
		std::vector<float> Splits;
	};
	struct CompiledRoadArm
	{
		glm::vec2 Direction = glm::vec2(1.0f, 0.0f);
		float Width = 24.0f;
		CityLayoutColor Color;
	};
	struct CompiledRoadArmGeometry
	{
		glm::vec2 Direction = glm::vec2(1.0f, 0.0f);
		glm::vec2 RightVertex = glm::vec2(0.0f);
		glm::vec2 LeftVertex = glm::vec2(0.0f);
		float Width = 24.0f;
		CityLayoutColor Color;
	};
	struct CompiledRoadNode
	{
		glm::vec2 Center = glm::vec2(0.0f);
		int SampleCount = 0;
		float MaxWidth = 24.0f;
		bool bJunction = false;
		CityLayoutColor Color;
		std::vector<CompiledRoadArm> Arms;
		std::vector<CompiledRoadArmGeometry> ArmGeometry;
		std::vector<glm::vec2> JunctionPolygon;
	};
	struct CompiledRoadSegment
	{
		size_t NodeA = 0;
		size_t NodeB = 0;
		float Width = 24.0f;
		CityLayoutColor Color;
	};
	auto cross2 = [](const glm::vec2& a, const glm::vec2& b) -> float
	{
		return a.x * b.y - a.y * b.x;
	};
	auto addSplit = [](std::vector<float>& splits, float t)
	{
		if (!std::isfinite(t))
			return;
		t = std::clamp(t, 0.0f, 1.0f);
		for (float existing : splits)
		{
			if (std::abs(existing - t) < 1.0e-4f)
				return;
		}
		splits.push_back(t);
	};
	auto roadColorForClass = [](const std::string& klass) -> CityLayoutColor
	{
		return klass == "arterial" ? CityLayoutColor{ 0.18f, 0.18f, 0.17f } : CityLayoutColor{ 0.20f, 0.20f, 0.19f };
	};

	std::vector<RawRoadSegment> rawRoadSegments;
	rawRoadSegments.reserve(CityDocument.Roads.size() * 16);
	for (const CityLayoutRoad& road : CityDocument.Roads)
	{
		for (size_t i = 1; i < road.Points.size(); ++i)
		{
			const glm::vec2 a(road.Points[i - 1].first, road.Points[i - 1].second);
			const glm::vec2 b(road.Points[i].first, road.Points[i].second);
			if (glm::length(b - a) < 1.0f)
				continue;
			RawRoadSegment segment;
			segment.A = a;
			segment.B = b;
			segment.Width = std::max(road.Width, 24.0f);
			segment.Color = roadColorForClass(road.Class);
			segment.Splits = { 0.0f, 1.0f };
			rawRoadSegments.push_back(std::move(segment));
		}
	}

	constexpr float kIntersectionTolerance = 2.0f;
	for (size_t i = 0; i < rawRoadSegments.size(); ++i)
	{
		RawRoadSegment& a = rawRoadSegments[i];
		const glm::vec2 r = a.B - a.A;
		const float rLenSq = glm::dot(r, r);
		if (rLenSq < 1.0f)
			continue;
		const glm::vec2 aMin = glm::min(a.A, a.B) - glm::vec2(kIntersectionTolerance);
		const glm::vec2 aMax = glm::max(a.A, a.B) + glm::vec2(kIntersectionTolerance);
		for (size_t j = i + 1; j < rawRoadSegments.size(); ++j)
		{
			RawRoadSegment& b = rawRoadSegments[j];
			const glm::vec2 bMin = glm::min(b.A, b.B) - glm::vec2(kIntersectionTolerance);
			const glm::vec2 bMax = glm::max(b.A, b.B) + glm::vec2(kIntersectionTolerance);
			if (aMax.x < bMin.x || bMax.x < aMin.x || aMax.y < bMin.y || bMax.y < aMin.y)
				continue;

			const glm::vec2 s = b.B - b.A;
			const float denom = cross2(r, s);
			if (std::abs(denom) < 1.0e-4f)
				continue;
			const glm::vec2 qp = b.A - a.A;
			const float t = cross2(qp, s) / denom;
			const float u = cross2(qp, r) / denom;
			if (t < -0.001f || t > 1.001f || u < -0.001f || u > 1.001f)
				continue;
			addSplit(a.Splits, t);
			addSplit(b.Splits, u);
		}
	}

	std::vector<CompiledRoadNode> roadNodes;
	std::vector<CompiledRoadSegment> compiledRoadSegments;
	std::unordered_map<std::string, size_t> roadNodeLookup;
	auto nodeKey = [](const glm::vec2& p) -> std::string
	{
		constexpr float kSnap = 4.0f;
		const long long x = static_cast<long long>(std::llround(p.x / kSnap));
		const long long y = static_cast<long long>(std::llround(p.y / kSnap));
		return std::to_string(x) + ":" + std::to_string(y);
	};
	auto addNode = [&](const glm::vec2& p, float width, CityLayoutColor color) -> size_t
	{
		const std::string key = nodeKey(p);
		auto it = roadNodeLookup.find(key);
		if (it != roadNodeLookup.end())
		{
			CompiledRoadNode& node = roadNodes[it->second];
			node.Center = (node.Center * static_cast<float>(node.SampleCount) + p) / static_cast<float>(node.SampleCount + 1);
			++node.SampleCount;
			if (width > node.MaxWidth)
			{
				node.MaxWidth = width;
				node.Color = color;
			}
			return it->second;
		}

		CompiledRoadNode node;
		node.Center = p;
		node.SampleCount = 1;
		node.MaxWidth = width;
		node.Color = color;
		const size_t index = roadNodes.size();
		roadNodeLookup.emplace(key, index);
		roadNodes.push_back(node);
		return index;
	};

	for (RawRoadSegment& raw : rawRoadSegments)
	{
		std::sort(raw.Splits.begin(), raw.Splits.end());
		raw.Splits.erase(
			std::unique(
				raw.Splits.begin(),
				raw.Splits.end(),
				[](float a, float b) { return std::abs(a - b) < 1.0e-4f; }),
			raw.Splits.end());
		for (size_t i = 1; i < raw.Splits.size(); ++i)
		{
			const float t0 = raw.Splits[i - 1];
			const float t1 = raw.Splits[i];
			if (t1 - t0 < 1.0e-4f)
				continue;
			const glm::vec2 a = raw.A + (raw.B - raw.A) * t0;
			const glm::vec2 b = raw.A + (raw.B - raw.A) * t1;
			if (glm::length(b - a) < 1.0f)
				continue;
			CompiledRoadSegment segment;
			segment.NodeA = addNode(a, raw.Width, raw.Color);
			segment.NodeB = addNode(b, raw.Width, raw.Color);
			segment.Width = raw.Width;
			segment.Color = raw.Color;
			compiledRoadSegments.push_back(segment);
		}
	}

	for (const CompiledRoadSegment& segment : compiledRoadSegments)
	{
		if (segment.NodeA >= roadNodes.size() || segment.NodeB >= roadNodes.size())
			continue;
		const glm::vec2 a = roadNodes[segment.NodeA].Center;
		const glm::vec2 b = roadNodes[segment.NodeB].Center;
		const glm::vec2 delta = b - a;
		const float length = glm::length(delta);
		if (length < 1.0f)
			continue;
		const glm::vec2 dir = delta / length;
		roadNodes[segment.NodeA].Arms.push_back({ dir, segment.Width, segment.Color });
		roadNodes[segment.NodeB].Arms.push_back({ -dir, segment.Width, segment.Color });
	}

	auto intersectLines2 = [&](const glm::vec2& originA, const glm::vec2& directionA, const glm::vec2& originB, const glm::vec2& directionB, glm::vec2& outPoint) -> bool
	{
		const float denom = cross2(directionA, directionB);
		if (std::abs(denom) < 1.0e-4f)
			return false;
		const float t = cross2(originB - originA, directionB) / denom;
		outPoint = originA + directionA * t;
		return std::isfinite(outPoint.x) && std::isfinite(outPoint.y);
	};

	for (CompiledRoadNode& node : roadNodes)
	{
		constexpr float kMergeSameDirectionDot = 0.9995f;
		constexpr float kExactStraightThroughDot = 0.9995f;
		std::vector<CompiledRoadArmGeometry> uniqueArms;
		for (const CompiledRoadArm& arm : node.Arms)
		{
			bool bMerged = false;
			for (CompiledRoadArmGeometry& existing : uniqueArms)
			{
				if (glm::dot(existing.Direction, arm.Direction) > kMergeSameDirectionDot)
				{
					existing.Direction = glm::normalize(existing.Direction + arm.Direction);
					if (arm.Width > existing.Width)
					{
						existing.Width = arm.Width;
						existing.Color = arm.Color;
					}
					bMerged = true;
					break;
				}
			}
			if (!bMerged)
			{
				CompiledRoadArmGeometry geometry;
				geometry.Direction = glm::normalize(arm.Direction);
				geometry.Width = arm.Width;
				geometry.Color = arm.Color;
				uniqueArms.push_back(geometry);
			}
		}

		if (uniqueArms.empty())
			continue;

		std::sort(
			uniqueArms.begin(),
			uniqueArms.end(),
			[](const CompiledRoadArmGeometry& a, const CompiledRoadArmGeometry& b)
			{
				return std::atan2(a.Direction.y, a.Direction.x) < std::atan2(b.Direction.y, b.Direction.x);
			});

		const bool bStraightThrough =
			uniqueArms.size() == 2u &&
			std::abs(glm::dot(uniqueArms[0].Direction, uniqueArms[1].Direction)) > kExactStraightThroughDot;
		node.bJunction = uniqueArms.size() >= 2u && !bStraightThrough;

		if (!node.bJunction)
		{
			for (CompiledRoadArmGeometry& arm : uniqueArms)
			{
				const glm::vec2 left(-arm.Direction.y, arm.Direction.x);
				const float halfWidth = std::max(arm.Width * 0.5f, 1.0f);
				arm.RightVertex = node.Center - left * halfWidth;
				arm.LeftVertex = node.Center + left * halfWidth;
			}
			node.ArmGeometry = std::move(uniqueArms);
			continue;
		}

		std::vector<glm::vec2> polygon;
		constexpr float kPi = 3.14159265358979323846f;
		for (size_t i = 0; i < uniqueArms.size(); ++i)
		{
			CompiledRoadArmGeometry& a = uniqueArms[i];
			CompiledRoadArmGeometry& b = uniqueArms[(i + 1) % uniqueArms.size()];
			const glm::vec2 leftA(-a.Direction.y, a.Direction.x);
			const glm::vec2 leftB(-b.Direction.y, b.Direction.x);
			float angleA = std::atan2(a.Direction.y, a.Direction.x);
			float angleB = std::atan2(b.Direction.y, b.Direction.x);
			if (i + 1 == uniqueArms.size())
				angleB += kPi * 2.0f;
			const float angleGap = angleB - angleA;
			const float halfWidthA = std::max(a.Width * 0.5f, 1.0f);
			const float halfWidthB = std::max(b.Width * 0.5f, 1.0f);
			const float maxHalfWidth = std::max(halfWidthA, halfWidthB);

			glm::vec2 corner(0.0f);
			const bool bSmallGap =
				angleGap < kPi - 0.02f &&
				intersectLines2(node.Center + leftA * halfWidthA, a.Direction, node.Center - leftB * halfWidthB, b.Direction, corner) &&
				glm::length(corner - node.Center) <= std::max(maxHalfWidth * 4.0f, 64.0f);
			if (bSmallGap)
			{
				a.LeftVertex = corner;
				b.RightVertex = corner;
				if (polygon.empty() || glm::length(corner - polygon.back()) >= 1.0f)
					polygon.push_back(corner);
			}
			else
			{
				const glm::vec2 aLeft = node.Center + a.Direction * halfWidthA + leftA * halfWidthA;
				const glm::vec2 bRight = node.Center + b.Direction * halfWidthB - leftB * halfWidthB;
				a.LeftVertex = aLeft;
				b.RightVertex = bRight;
				if (polygon.empty() || glm::length(aLeft - polygon.back()) >= 1.0f)
					polygon.push_back(aLeft);
				if (polygon.empty() || glm::length(bRight - polygon.back()) >= 1.0f)
					polygon.push_back(bRight);
			}
		}
		if (polygon.size() > 1 && glm::length(polygon.front() - polygon.back()) < 1.0f)
			polygon.pop_back();
		node.JunctionPolygon = std::move(polygon);
		node.ArmGeometry = std::move(uniqueArms);
	}

	auto findArmGeometry = [](const CompiledRoadNode& node, const glm::vec2& direction) -> const CompiledRoadArmGeometry*
	{
		const CompiledRoadArmGeometry* best = nullptr;
		float bestDot = -1.0f;
		for (const CompiledRoadArmGeometry& arm : node.ArmGeometry)
		{
			const float dot = glm::dot(arm.Direction, direction);
			if (dot > bestDot)
			{
				bestDot = dot;
				best = &arm;
			}
		}
		return bestDot > 0.90f ? best : nullptr;
	};

	int roadIndex = 1;
	for (const CompiledRoadSegment& segment : compiledRoadSegments)
	{
		if (segment.NodeA >= roadNodes.size() || segment.NodeB >= roadNodes.size())
			continue;
		const CompiledRoadNode& nodeA = roadNodes[segment.NodeA];
		const CompiledRoadNode& nodeB = roadNodes[segment.NodeB];
		glm::vec2 a = nodeA.Center;
		glm::vec2 b = nodeB.Center;
		const glm::vec2 delta = b - a;
		const float fullLength = glm::length(delta);
		if (fullLength < 1.0f)
			continue;
		const glm::vec2 dir = delta / fullLength;
		const CompiledRoadArmGeometry* armA = findArmGeometry(nodeA, dir);
		const CompiledRoadArmGeometry* armB = findArmGeometry(nodeB, -dir);
		if (!armA || !armB)
			continue;

		const std::vector<glm::vec2> polygon = {
			armA->RightVertex,
			armB->LeftVertex,
			armB->RightVertex,
			armA->LeftVertex
		};
		const float len = fullLength;
		if (len < 1.0f)
			continue;
		const float yaw = -std::atan2(dir.y, dir.x) * 57.2957795f;
		writeRoadPolygonDecal("Road_" + std::to_string(roadIndex++), segment.Color,
			(a.x + b.x) * 0.5f, 5.52f, (a.y + b.y) * 0.5f,
			yaw,
			len, segment.Width,
			polygon,
			0.88f,
			6.0f, std::max(1.0f, segment.Width / 180.0f),
			true);
	}

	int junctionIndex = 1;
	for (const CompiledRoadNode& node : roadNodes)
	{
		if (!node.bJunction || node.JunctionPolygon.size() < 3)
			continue;
		std::vector<glm::vec2> polygon = node.JunctionPolygon;
		if (polygon.size() < 3)
			continue;
		if (polygon.size() > Corona::RoadDecalState::MaxPolygonVertices)
		{
			std::vector<glm::vec2> reduced;
			reduced.reserve(Corona::RoadDecalState::MaxPolygonVertices);
			for (size_t i = 0; i < Corona::RoadDecalState::MaxPolygonVertices; ++i)
				reduced.push_back(polygon[(i * polygon.size()) / Corona::RoadDecalState::MaxPolygonVertices]);
			polygon = std::move(reduced);
		}

		glm::vec2 boundsMin(std::numeric_limits<float>::max());
		glm::vec2 boundsMax(-std::numeric_limits<float>::max());
		for (const glm::vec2& point : polygon)
		{
			boundsMin = glm::min(boundsMin, point);
			boundsMax = glm::max(boundsMax, point);
		}
		const float size = std::max(std::max(boundsMax.x - boundsMin.x, boundsMax.y - boundsMin.y), node.MaxWidth);
		writeRoadPolygonDecal(
			"RoadJunction_" + std::to_string(junctionIndex++),
			node.Color,
			node.Center.x,
			5.54f,
			node.Center.y,
			0.0f,
			size,
			size,
			polygon,
			0.88f,
			std::max(size / 180.0f, 1.0f),
			std::max(size / 180.0f, 1.0f),
			false);
	}

	for (const CityLayoutBuilding& b : CityDocument.Buildings)
	{
		// Corona BOX transforms use position.y as the bottom, not the center.
		// Sink proportionally to height so tall silhouettes do not reveal base seams
		// from oblique views or after temporal reconstruction.
		const float buildingBaseSink = std::clamp(b.Height * 0.08f, 40.0f, 256.0f);
		writeBox("Building_" + b.Id + "_" + b.Type, b.Color, b.bBrickTexture, 4.0f,
			b.X, -buildingBaseSink, b.Y,
			0.0f, b.Rotation, 0.0f,
			b.Width, b.Height + buildingBaseSink, b.Depth, 0.78f);
	}

	file << "    {\n";
	file << "      name = \"Sun\",\n";
	file << "      light = { type = \"directional\", direction = {0.35,0.85,0.38}, color = {1,0.94,0.84}, intensity = 2.2, cast_shadow = true, enabled = true },\n";
	file << "    },\n";
	file << "    {\n";
	file << "      name = \"City_Key_Light\",\n";
	file << "      light = { type = \"point\", position = {0,900,0}, color = {0.7,0.82,1.0}, intensity = 9.0, radius = 4200, cast_shadow = true, enabled = true },\n";
	file << "    },\n";
	file << "  },\n";
	file << "}\n";
	file.close();

	Host->QueueEditorMapLoad(PlatformUtf8ToWide(mapName));
	PendingErrorMessage = "compiled and queued map: assets/maps/" + mapName + ".map";
	AppendCpuRuntimeTrace(L"[AssetExplorer] compiled city layout to map " + mapPath.wstring());
	return true;
}

void CoronaAssetExplorer::RenderRoadGraphViewer()
{
	if (!bRoadGraphViewerVisible)
		return;

	std::string title = "Procedural Road Graph";
	if (RoadDocument.bLoaded && !RoadDocument.SourcePath.empty())
		title += " - " + RoadDocument.SourcePath.filename().string();
	title += "###road_graph_viewer";

	ImGui::SetNextWindowSize(ImVec2(1120.0f, 760.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin(title.c_str(), &bRoadGraphViewerVisible))
	{
		ImGui::End();
		return;
	}

	if (!RoadDocument.bLoaded)
	{
		if (ImGui::Button("Generate Road Graph"))
			CreateProceduralRoadGraph();
		ImGui::End();
		return;
	}

	bool regenerate = false;
	if (ImGui::Button("Generate"))
		regenerate = true;
	ImGui::SameLine();
	if (ImGui::Button("Save"))
		SaveRoadGraphAsset();
	ImGui::SameLine();
	if (ImGui::Button("Fit"))
	{
		bRoadGraphAutoFit = true;
		RoadGraphZoom = 1.0f;
		RoadGraphPanX = 0.0f;
		RoadGraphPanY = 0.0f;
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(150.0f);
	if (ImGui::SliderFloat("Zoom##road", &RoadGraphZoom, 0.2f, 4.0f, "%.2f"))
		bRoadGraphAutoFit = false;
	ImGui::SameLine();
	ImGui::Checkbox("Nodes", &bRoadGraphShowNodes);
	ImGui::SameLine();
	if (bRoadGraphDirty)
		ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.36f, 1.0f), "Dirty");
	else
		ImGui::TextDisabled("%zu nodes, %zu roads", RoadDocument.Nodes.size(), RoadDocument.Edges.size());

	RoadGraphParams pending = RoadDocument.Params;
	ImGui::SetNextItemWidth(90.0f);
	regenerate = ImGui::InputInt("Seed", &pending.Seed) || regenerate;
	ImGui::SameLine();
	ImGui::SetNextItemWidth(120.0f);
	regenerate = ImGui::SliderFloat("Density", &pending.Density, 0.05f, 1.0f, "%.2f") || regenerate;
	ImGui::SameLine();
	ImGui::SetNextItemWidth(120.0f);
	regenerate = ImGui::SliderFloat("Curvature", &pending.Curvature, 0.0f, 1.0f, "%.2f") || regenerate;
	ImGui::SameLine();
	ImGui::SetNextItemWidth(120.0f);
	regenerate = ImGui::SliderFloat("Branching", &pending.Branching, 0.0f, 1.0f, "%.2f") || regenerate;
	ImGui::SameLine();
	ImGui::SetNextItemWidth(90.0f);
	regenerate = ImGui::InputInt("Arterials", &pending.Arterials) || regenerate;
	ImGui::SetNextItemWidth(130.0f);
	regenerate = ImGui::SliderFloat("Width", &pending.Width, 20000.0f, 240000.0f, "%.0f") || regenerate;
	ImGui::SameLine();
	ImGui::SetNextItemWidth(130.0f);
	regenerate = ImGui::SliderFloat("Height", &pending.Height, 20000.0f, 240000.0f, "%.0f") || regenerate;
	if (regenerate)
	{
		pending.Density = std::clamp(pending.Density, 0.05f, 1.0f);
		pending.Curvature = std::clamp(pending.Curvature, 0.0f, 1.0f);
		pending.Branching = std::clamp(pending.Branching, 0.0f, 1.0f);
		pending.Arterials = std::clamp(pending.Arterials, 2, 24);
		RoadDocument.Params = pending;
		GenerateProceduralRoadGraph();
		bRoadGraphAutoFit = true;
	}
	ImGui::Separator();

	ImGui::BeginChild("##road_graph_canvas", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImVec2 canvasPos = ImGui::GetCursorScreenPos();
	ImVec2 canvasSize = ImGui::GetContentRegionAvail();
	canvasSize.x = std::max(canvasSize.x, 240.0f);
	canvasSize.y = std::max(canvasSize.y, 240.0f);
	const ImVec2 canvasMax(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->AddRectFilled(canvasPos, canvasMax, IM_COL32(10, 13, 16, 255));
	draw->AddRect(canvasPos, canvasMax, IM_COL32(78, 88, 102, 255));

	ImGui::InvisibleButton("##road_graph_canvas_button", canvasSize, ImGuiButtonFlags_MouseButtonRight);
	const bool canvasHovered = ImGui::IsItemHovered();
	const ImGuiIO& io = ImGui::GetIO();
	if (canvasHovered && io.MouseWheel != 0.0f)
	{
		bRoadGraphAutoFit = false;
		RoadGraphZoom = std::clamp(RoadGraphZoom * (io.MouseWheel > 0.0f ? 1.12f : 0.89f), 0.2f, 4.0f);
	}
	if (canvasHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Right))
	{
		bRoadGraphAutoFit = false;
		RoadGraphPanX += io.MouseDelta.x;
		RoadGraphPanY += io.MouseDelta.y;
	}

	float minX = std::numeric_limits<float>::max();
	float minY = std::numeric_limits<float>::max();
	float maxX = std::numeric_limits<float>::lowest();
	float maxY = std::numeric_limits<float>::lowest();
	auto includePoint = [&](float x, float y)
	{
		minX = std::min(minX, x);
		minY = std::min(minY, y);
		maxX = std::max(maxX, x);
		maxY = std::max(maxY, y);
	};
	for (const RoadGraphNode& node : RoadDocument.Nodes)
		includePoint(node.X, node.Y);
	for (const RoadGraphEdge& edge : RoadDocument.Edges)
	{
		for (const auto& p : edge.Points)
			includePoint(p.first, p.second);
	}
	if (minX == std::numeric_limits<float>::max())
	{
		minX = -100.0f; minY = -100.0f; maxX = 100.0f; maxY = 100.0f;
	}
	const float boundsW = std::max(maxX - minX, 1.0f);
	const float boundsH = std::max(maxY - minY, 1.0f);
	const float centerX = (minX + maxX) * 0.5f;
	const float centerY = (minY + maxY) * 0.5f;
	const float fitScale = std::min(canvasSize.x / (boundsW * 1.15f), canvasSize.y / (boundsH * 1.15f));
	const float scale = std::max(0.0001f, fitScale * RoadGraphZoom);
	if (bRoadGraphAutoFit)
	{
		RoadGraphPanX = 0.0f;
		RoadGraphPanY = 0.0f;
	}
	const ImVec2 canvasCenter(canvasPos.x + canvasSize.x * 0.5f + RoadGraphPanX, canvasPos.y + canvasSize.y * 0.5f + RoadGraphPanY);
	auto worldToScreen = [&](float x, float y) -> ImVec2
	{
		return ImVec2(canvasCenter.x + (x - centerX) * scale, canvasCenter.y - (y - centerY) * scale);
	};
	std::unordered_map<std::string, std::pair<float, float>> nodePositions;
	for (const RoadGraphNode& node : RoadDocument.Nodes)
		nodePositions[node.Id] = { node.X, node.Y };

	for (int gx = -8; gx <= 8; ++gx)
	{
		const float t = gx / 8.0f;
		const float x = canvasPos.x + canvasSize.x * (0.5f + t * 0.5f);
		const float y = canvasPos.y + canvasSize.y * (0.5f + t * 0.5f);
		draw->AddLine(ImVec2(x, canvasPos.y), ImVec2(x, canvasMax.y), IM_COL32(255, 255, 255, gx == 0 ? 24 : 10));
		draw->AddLine(ImVec2(canvasPos.x, y), ImVec2(canvasMax.x, y), IM_COL32(255, 255, 255, gx == 0 ? 24 : 10));
	}

	auto roadColor = [](const std::string& klass) -> ImU32
	{
		if (klass == "arterial")
			return IM_COL32(218, 204, 156, 250);
		if (klass == "collector")
			return IM_COL32(172, 184, 190, 245);
		return IM_COL32(118, 128, 136, 235);
	};
	for (const RoadGraphEdge& edge : RoadDocument.Edges)
	{
		std::vector<std::pair<float, float>> points = edge.Points;
		if (points.empty())
		{
			auto fromIt = nodePositions.find(edge.From);
			auto toIt = nodePositions.find(edge.To);
			if (fromIt == nodePositions.end() || toIt == nodePositions.end())
				continue;
			points = { fromIt->second, toIt->second };
		}
		std::vector<ImVec2> screenPoints;
		screenPoints.reserve(points.size());
		for (const auto& p : points)
			screenPoints.push_back(worldToScreen(p.first, p.second));
		if (screenPoints.size() < 2)
			continue;
		const float thickness = std::clamp(edge.Width * scale, 2.0f, 34.0f);
		draw->AddPolyline(screenPoints.data(), static_cast<int>(screenPoints.size()), IM_COL32(18, 20, 23, 245), 0, thickness + 5.0f);
		draw->AddPolyline(screenPoints.data(), static_cast<int>(screenPoints.size()), roadColor(edge.Class), 0, thickness);
	}
	if (bRoadGraphShowNodes)
	{
		for (const RoadGraphNode& node : RoadDocument.Nodes)
		{
			const ImVec2 p = worldToScreen(node.X, node.Y);
			const ImU32 color = node.Kind == "arterial" ? IM_COL32(255, 236, 172, 245) :
				(node.Kind == "collector" ? IM_COL32(198, 215, 220, 230) : IM_COL32(150, 162, 172, 220));
			draw->AddCircleFilled(p, node.Kind == "arterial" ? 4.5f : 3.0f, color);
		}
	}
	ImGui::EndChild();
	ImGui::End();
}

void CoronaAssetExplorer::RenderCityLayoutViewer()
{
	if (!bCityLayoutViewerVisible)
		return;

	std::string title = "City Layout";
	if (CityDocument.bLoaded && !CityDocument.SourcePath.empty())
		title += " - " + CityDocument.SourcePath.filename().string();
	title += "###city_layout_viewer";

	ImGui::SetNextWindowSize(ImVec2(1180.0f, 780.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin(title.c_str(), &bCityLayoutViewerVisible))
	{
		ImGui::End();
		return;
	}

	if (!CityDocument.bLoaded)
	{
		ImGui::TextDisabled("Open a .citylayout file from the Asset Explorer.");
		ImGui::End();
		return;
	}

	if (ImGui::Button("Generate Roads"))
		GenerateCityLayoutRoads();
	ImGui::SameLine();
	if (ImGui::Button("Rebuild Buildings"))
	{
		RebuildCityLayoutBuildings();
		bCityLayoutDirty = true;
	}
	ImGui::SameLine();
	if (ImGui::Button("Save"))
		SaveCityLayoutAsset();
	ImGui::SameLine();
	if (ImGui::Button("Compile + Load Map"))
		CompileCityLayoutToMapAndLoad();
	ImGui::SameLine();
	if (ImGui::Button("Fit"))
	{
		bCityLayoutAutoFit = true;
		CityLayoutZoom = 1.0f;
		CityLayoutPanX = 0.0f;
		CityLayoutPanY = 0.0f;
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(140.0f);
	if (ImGui::SliderFloat("Zoom##city", &CityLayoutZoom, 0.2f, 4.0f, "%.2f"))
		bCityLayoutAutoFit = false;
	ImGui::SameLine();
	ImGui::Checkbox("Buildings", &bCityLayoutShowBuildings);
	ImGui::SameLine();
	if (bCityLayoutDirty)
		ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.36f, 1.0f), "Dirty");
	else
		ImGui::TextDisabled("%zu roads, %zu buildings", CityDocument.Roads.size(), CityDocument.Buildings.size());

	bool rebuildBuildings = false;
	CityLayoutParams pending = CityDocument.Params;
	ImGui::SetNextItemWidth(82.0f);
	if (ImGui::InputInt("Seed##city", &pending.Seed))
		rebuildBuildings = true;
	ImGui::SameLine();
	ImGui::SetNextItemWidth(112.0f);
	if (ImGui::SliderFloat("Density##city", &pending.BuildingDensity, 0.0f, 1.0f, "%.2f"))
		rebuildBuildings = true;
	ImGui::SameLine();
	ImGui::SetNextItemWidth(112.0f);
	if (ImGui::SliderFloat("Lot##city", &pending.LotSize, 260.0f, 1200.0f, "%.0f"))
		rebuildBuildings = true;
	ImGui::SameLine();
	ImGui::SetNextItemWidth(112.0f);
	if (ImGui::SliderFloat("Size variance##city", &pending.BuildingSizeVariance, 0.0f, 2.0f, "%.2f"))
		rebuildBuildings = true;
	ImGui::SameLine();
	ImGui::SetNextItemWidth(112.0f);
	if (ImGui::SliderFloat("Setback##city", &pending.Setback, 20.0f, 500.0f, "%.0f"))
		rebuildBuildings = true;
	ImGui::SameLine();
	ImGui::SetNextItemWidth(112.0f);
	if (ImGui::SliderFloat("Road width##city", &pending.RoadWidth, 120.0f, 680.0f, "%.0f"))
		rebuildBuildings = true;
	ImGui::SetNextItemWidth(112.0f);
	ImGui::InputFloat("Width##city", &pending.Width, 0.0f, 0.0f, "%.0f");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(112.0f);
	ImGui::InputFloat("Depth##city", &pending.Depth, 0.0f, 0.0f, "%.0f");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(112.0f);
	ImGui::InputFloat("Road spacing##city", &pending.RoadSpacing, 0.0f, 0.0f, "%.0f");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(112.0f);
	ImGui::InputFloat("Road jitter##city", &pending.RoadJitter, 0.0f, 0.0f, "%.0f");
	if (pending.Width != CityDocument.Params.Width ||
		pending.Depth != CityDocument.Params.Depth ||
		pending.RoadSpacing != CityDocument.Params.RoadSpacing ||
		pending.RoadJitter != CityDocument.Params.RoadJitter)
	{
		bCityLayoutDirty = true;
	}
	if (rebuildBuildings ||
		pending.Seed != CityDocument.Params.Seed ||
		pending.Width != CityDocument.Params.Width ||
		pending.Depth != CityDocument.Params.Depth ||
		pending.BuildingDensity != CityDocument.Params.BuildingDensity ||
		pending.BuildingSizeVariance != CityDocument.Params.BuildingSizeVariance ||
		pending.LotSize != CityDocument.Params.LotSize ||
		pending.Setback != CityDocument.Params.Setback ||
		pending.RoadWidth != CityDocument.Params.RoadWidth)
	{
		pending.Width = std::max(1000.0f, pending.Width);
		pending.Depth = std::max(1000.0f, pending.Depth);
		pending.RoadSpacing = std::max(500.0f, pending.RoadSpacing);
		pending.RoadJitter = std::max(0.0f, pending.RoadJitter);
		pending.RoadWidth = std::max(80.0f, pending.RoadWidth);
		pending.BuildingSizeVariance = std::clamp(pending.BuildingSizeVariance, 0.0f, 2.0f);
		CityDocument.Params = pending;
		for (CityLayoutRoad& road : CityDocument.Roads)
		{
			if (road.Class != "arterial")
				road.Width = CityDocument.Params.RoadWidth;
		}
		RebuildCityLayoutBuildings();
		bCityLayoutDirty = true;
	}
	else
	{
		CityDocument.Params = pending;
	}

	if (!PendingErrorMessage.empty())
		ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.55f, 1.0f), "%s", PendingErrorMessage.c_str());
	if (!CityDocument.Error.empty())
		ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.55f, 1.0f), "%s", CityDocument.Error.c_str());

	ImGui::Separator();
	ImGui::BeginChild("##city_layout_canvas", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImVec2 canvasPos = ImGui::GetCursorScreenPos();
	ImVec2 canvasSize = ImGui::GetContentRegionAvail();
	canvasSize.x = std::max(canvasSize.x, 260.0f);
	canvasSize.y = std::max(canvasSize.y, 260.0f);
	const ImVec2 canvasMax(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->AddRectFilled(canvasPos, canvasMax, IM_COL32(9, 12, 15, 255));
	draw->AddRect(canvasPos, canvasMax, IM_COL32(78, 88, 102, 255));

	ImGui::InvisibleButton("##city_canvas_button", canvasSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
	const bool canvasHovered = ImGui::IsItemHovered();
	const ImGuiIO& io = ImGui::GetIO();
	if (canvasHovered && io.MouseWheel != 0.0f)
	{
		bCityLayoutAutoFit = false;
		CityLayoutZoom = std::clamp(CityLayoutZoom * (io.MouseWheel > 0.0f ? 1.12f : 0.89f), 0.2f, 4.0f);
	}
	if (canvasHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Right))
	{
		bCityLayoutAutoFit = false;
		CityLayoutPanX += io.MouseDelta.x;
		CityLayoutPanY += io.MouseDelta.y;
	}

	const float hx = CityDocument.Params.Width * 0.5f;
	const float hy = CityDocument.Params.Depth * 0.5f;
	float minX = -hx;
	float minY = -hy;
	float maxX = hx;
	float maxY = hy;
	for (const CityLayoutRoad& road : CityDocument.Roads)
	{
		for (const auto& p : road.Points)
		{
			minX = std::min(minX, p.first);
			minY = std::min(minY, p.second);
			maxX = std::max(maxX, p.first);
			maxY = std::max(maxY, p.second);
		}
	}
	const float boundsW = std::max(maxX - minX, 1.0f);
	const float boundsH = std::max(maxY - minY, 1.0f);
	const float centerX = (minX + maxX) * 0.5f;
	const float centerY = (minY + maxY) * 0.5f;
	const float fitScale = std::min(canvasSize.x / (boundsW * 1.12f), canvasSize.y / (boundsH * 1.12f));
	const float scale = std::max(0.0001f, fitScale * CityLayoutZoom);
	if (bCityLayoutAutoFit)
	{
		CityLayoutPanX = 0.0f;
		CityLayoutPanY = 0.0f;
	}
	const ImVec2 canvasCenter(canvasPos.x + canvasSize.x * 0.5f + CityLayoutPanX, canvasPos.y + canvasSize.y * 0.5f + CityLayoutPanY);
	auto worldToScreen = [&](float x, float y) -> ImVec2
	{
		return ImVec2(canvasCenter.x + (x - centerX) * scale, canvasCenter.y - (y - centerY) * scale);
	};
	auto screenToWorld = [&](ImVec2 p) -> std::pair<float, float>
	{
		return { centerX + (p.x - canvasCenter.x) / scale, centerY - (p.y - canvasCenter.y) / scale };
	};
	auto distanceToScreenSegmentSq = [](ImVec2 p, ImVec2 a, ImVec2 b, float* outT = nullptr) -> float
	{
		const float vx = b.x - a.x;
		const float vy = b.y - a.y;
		const float wx = p.x - a.x;
		const float wy = p.y - a.y;
		const float lenSq = vx * vx + vy * vy;
		const float t = lenSq > 1.0e-5f ? std::clamp((wx * vx + wy * vy) / lenSq, 0.0f, 1.0f) : 0.0f;
		if (outT)
			*outT = t;
		const float dx = p.x - (a.x + vx * t);
		const float dy = p.y - (a.y + vy * t);
		return dx * dx + dy * dy;
	};

	for (int gx = -10; gx <= 10; ++gx)
	{
		const float t = gx / 10.0f;
		const float x = canvasPos.x + canvasSize.x * (0.5f + t * 0.5f);
		const float y = canvasPos.y + canvasSize.y * (0.5f + t * 0.5f);
		draw->AddLine(ImVec2(x, canvasPos.y), ImVec2(x, canvasMax.y), IM_COL32(255, 255, 255, gx == 0 ? 24 : 9));
		draw->AddLine(ImVec2(canvasPos.x, y), ImVec2(canvasMax.x, y), IM_COL32(255, 255, 255, gx == 0 ? 24 : 9));
	}
	const ImVec2 boundsA = worldToScreen(-hx, hy);
	const ImVec2 boundsB = worldToScreen(hx, -hy);
	draw->AddRect(boundsA, boundsB, IM_COL32(104, 124, 146, 200), 0.0f, 0, 1.5f);

	if (bCityLayoutShowBuildings)
	{
		for (const CityLayoutBuilding& b : CityDocument.Buildings)
		{
			const ImVec2 a = worldToScreen(b.X - b.Width * 0.5f, b.Y + b.Depth * 0.5f);
			const ImVec2 c = worldToScreen(b.X + b.Width * 0.5f, b.Y - b.Depth * 0.5f);
			const ImU32 fill = IM_COL32(
				static_cast<int>(std::clamp(b.Color.R, 0.0f, 1.0f) * 255.0f),
				static_cast<int>(std::clamp(b.Color.G, 0.0f, 1.0f) * 255.0f),
				static_cast<int>(std::clamp(b.Color.B, 0.0f, 1.0f) * 255.0f),
				120);
			draw->AddRectFilled(a, c, fill, 1.0f);
			draw->AddRect(a, c, IM_COL32(208, 218, 226, 80), 1.0f);
		}
	}

	auto roadColor = [](const std::string& klass, bool selected) -> ImU32
	{
		if (selected)
			return IM_COL32(255, 224, 114, 255);
		if (klass == "arterial")
			return IM_COL32(218, 202, 154, 255);
		return IM_COL32(145, 156, 166, 245);
	};
	for (int roadIndex = 0; roadIndex < static_cast<int>(CityDocument.Roads.size()); ++roadIndex)
	{
		const CityLayoutRoad& road = CityDocument.Roads[roadIndex];
		if (road.Points.size() < 2)
			continue;
		std::vector<ImVec2> screenPoints;
		screenPoints.reserve(road.Points.size());
		for (const auto& p : road.Points)
			screenPoints.push_back(worldToScreen(p.first, p.second));
		const bool selected = roadIndex == SelectedCityRoad;
		const float thickness = std::clamp(road.Width * scale, 3.0f, 48.0f);
		draw->AddPolyline(screenPoints.data(), static_cast<int>(screenPoints.size()), IM_COL32(16, 18, 20, 255), 0, thickness + 6.0f);
		draw->AddPolyline(screenPoints.data(), static_cast<int>(screenPoints.size()), roadColor(road.Class, selected), 0, thickness);
		for (int vertexIndex = 0; vertexIndex < static_cast<int>(screenPoints.size()); ++vertexIndex)
		{
			const ImVec2 p = screenPoints[vertexIndex];
			draw->AddCircleFilled(p, selected ? 5.0f : 3.5f, selected ? IM_COL32(255, 236, 146, 255) : IM_COL32(220, 228, 232, 220));
			if (selected)
				draw->AddCircle(p, 7.0f, IM_COL32(20, 22, 24, 230), 16, 1.2f);
		}
	}

	if (DraggingCityRoad >= 0 && DraggingCityRoad < static_cast<int>(CityDocument.Roads.size()) &&
		DraggingCityVertex >= 0 && DraggingCityVertex < static_cast<int>(CityDocument.Roads[DraggingCityRoad].Points.size()))
	{
		if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			const auto world = screenToWorld(io.MousePos);
			CityDocument.Roads[DraggingCityRoad].Points[DraggingCityVertex] = {
				std::clamp(world.first, -hx, hx),
				std::clamp(world.second, -hy, hy)
			};
			RebuildCityLayoutBuildings();
			bCityLayoutDirty = true;
		}
		else
		{
			DraggingCityRoad = -1;
			DraggingCityVertex = -1;
		}
	}
	else if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		int hitRoad = -1;
		int hitVertex = -1;
		float hitDistSq = 100.0f;
		for (int roadIndex = 0; roadIndex < static_cast<int>(CityDocument.Roads.size()); ++roadIndex)
		{
			const CityLayoutRoad& road = CityDocument.Roads[roadIndex];
			for (int vertexIndex = 0; vertexIndex < static_cast<int>(road.Points.size()); ++vertexIndex)
			{
				const ImVec2 p = worldToScreen(road.Points[vertexIndex].first, road.Points[vertexIndex].second);
				const float dx = io.MousePos.x - p.x;
				const float dy = io.MousePos.y - p.y;
				const float d = dx * dx + dy * dy;
				if (d < hitDistSq)
				{
					hitDistSq = d;
					hitRoad = roadIndex;
					hitVertex = vertexIndex;
				}
			}
		}
		if (hitRoad >= 0)
		{
			SelectedCityRoad = hitRoad;
			DraggingCityRoad = hitRoad;
			DraggingCityVertex = hitVertex;
		}
		else
		{
			int insertRoad = -1;
			int insertAfter = -1;
			float insertT = 0.0f;
			float bestSegment = 144.0f;
			for (int roadIndex = 0; roadIndex < static_cast<int>(CityDocument.Roads.size()); ++roadIndex)
			{
				const CityLayoutRoad& road = CityDocument.Roads[roadIndex];
				for (int i = 1; i < static_cast<int>(road.Points.size()); ++i)
				{
					float t = 0.0f;
					const ImVec2 a = worldToScreen(road.Points[i - 1].first, road.Points[i - 1].second);
					const ImVec2 b = worldToScreen(road.Points[i].first, road.Points[i].second);
					const float d = distanceToScreenSegmentSq(io.MousePos, a, b, &t);
					if (d < bestSegment)
					{
						bestSegment = d;
						insertRoad = roadIndex;
						insertAfter = i - 1;
						insertT = t;
					}
				}
			}
			if (insertRoad >= 0 && insertAfter >= 0)
			{
				CityLayoutRoad& road = CityDocument.Roads[insertRoad];
				const auto& a = road.Points[insertAfter];
				const auto& b = road.Points[insertAfter + 1];
				const std::pair<float, float> p = {
					a.first + (b.first - a.first) * insertT,
					a.second + (b.second - a.second) * insertT
				};
				road.Points.insert(road.Points.begin() + insertAfter + 1, p);
				SelectedCityRoad = insertRoad;
				DraggingCityRoad = insertRoad;
				DraggingCityVertex = insertAfter + 1;
				RebuildCityLayoutBuildings();
				bCityLayoutDirty = true;
			}
		}
	}

	draw->AddText(ImVec2(canvasPos.x + 10.0f, canvasMax.y - 24.0f), IM_COL32(188, 198, 208, 205),
		"Left-drag road vertices / Click road segment to insert vertex / Mouse wheel zoom / Right-drag pan");
	ImGui::EndChild();
	ImGui::End();
}

void CoronaAssetExplorer::RenderWorldLayoutViewer()
{
	if (!bWorldLayoutViewerVisible)
		return;

	std::string title = "World Layout Graph";
	if (LayoutDocument.bLoaded)
		title += " - " + LayoutDocument.SourcePath.filename().string();
	title += "###world_layout_graph_viewer";

	ImGui::SetNextWindowSize(ImVec2(1120.0f, 760.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin(title.c_str(), &bWorldLayoutViewerVisible))
	{
		ImGui::End();
		return;
	}

	if (!LayoutDocument.bLoaded)
	{
		ImGui::TextDisabled("No .worldlayout file is loaded.");
		ImGui::End();
		return;
	}

	if (ImGui::Button("Reload"))
		OpenWorldLayoutAsset(LayoutDocument.SourcePath);
	ImGui::SameLine();
	if (ImGui::Button("Save"))
		SaveWorldLayoutAsset();
	ImGui::SameLine();
	if (ImGui::Button("Fit"))
	{
		bWorldLayoutAutoFit = true;
		WorldLayoutZoom = 1.0f;
		WorldLayoutPanX = 0.0f;
		WorldLayoutPanY = 0.0f;
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(160.0f);
	ImGui::SliderFloat("Zoom", &WorldLayoutZoom, 0.2f, 4.0f, "%.2f");
	ImGui::SameLine();
	ImGui::Checkbox("Boundary roads", &bWorldLayoutShowImplicitRoads);
	ImGui::SameLine();
	if (bWorldLayoutDirty)
	{
		ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.36f, 1.0f), "Dirty");
		ImGui::SameLine();
	}
	ImGui::TextDisabled("%zu nodes", LayoutDocument.Nodes.size());
	ImGui::Separator();

	const float leftWidth = 300.0f;
	ImGui::BeginChild("##world_layout_details", ImVec2(leftWidth, 0.0f), true);
	ImGui::TextColored(ImVec4(0.92f, 0.88f, 0.70f, 1.0f), "%s", LayoutDocument.Name.c_str());
	ImGui::TextDisabled("%s", LayoutDocument.SourcePath.string().c_str());
	if (!LayoutDocument.Error.empty())
	{
		ImGui::Separator();
		ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1.0f), "Parse warnings:");
		ImGui::TextWrapped("%s", LayoutDocument.Error.c_str());
	}
	ImGui::Separator();
	ImGui::Text("Nodes");
	std::unordered_map<std::string, int> listNodeIndex;
	listNodeIndex.reserve(LayoutDocument.Nodes.size());
	for (int i = 0; i < static_cast<int>(LayoutDocument.Nodes.size()); ++i)
		listNodeIndex[LayoutDocument.Nodes[i].Id] = i;
	std::vector<std::vector<int>> childNodes(LayoutDocument.Nodes.size());
	std::vector<int> rootNodes;
	rootNodes.reserve(LayoutDocument.Nodes.size());
	for (int i = 0; i < static_cast<int>(LayoutDocument.Nodes.size()); ++i)
	{
		const WorldLayoutNode& node = LayoutDocument.Nodes[i];
		auto parentIt = listNodeIndex.find(node.Parent);
		if (node.Parent.empty() || parentIt == listNodeIndex.end() || parentIt->second == i)
			rootNodes.push_back(i);
		else
			childNodes[parentIt->second].push_back(i);
	}
	if (rootNodes.empty())
	{
		for (int i = 0; i < static_cast<int>(LayoutDocument.Nodes.size()); ++i)
			rootNodes.push_back(i);
	}
	std::unordered_set<int> treeVisitStack;
	auto renderNodeTree = [&](auto&& self, int i) -> void
	{
		if (i < 0 || i >= static_cast<int>(LayoutDocument.Nodes.size()))
			return;
		if (!treeVisitStack.insert(i).second)
			return;

		const WorldLayoutNode& node = LayoutDocument.Nodes[i];
		const bool hasChildren = !childNodes[i].empty();
		ImGuiTreeNodeFlags flags =
			ImGuiTreeNodeFlags_OpenOnArrow |
			ImGuiTreeNodeFlags_SpanAvailWidth |
			(SelectedWorldLayoutNode == i ? ImGuiTreeNodeFlags_Selected : 0);
		if (!hasChildren)
			flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
		else if (node.Parent.empty())
			flags |= ImGuiTreeNodeFlags_DefaultOpen;

		const std::string label = node.Type + " / " + node.Label + "##tree_" + node.Id;
		const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
			SelectedWorldLayoutNode = i;
		if (hasChildren && open)
		{
			for (int child : childNodes[i])
				self(self, child);
			ImGui::TreePop();
		}
		treeVisitStack.erase(i);
	};
	for (int root : rootNodes)
		renderNodeTree(renderNodeTree, root);
	if (SelectedWorldLayoutNode >= 0 && SelectedWorldLayoutNode < static_cast<int>(LayoutDocument.Nodes.size()))
	{
		const WorldLayoutNode& node = LayoutDocument.Nodes[SelectedWorldLayoutNode];
		ImGui::Separator();
		ImGui::Text("Selected");
		ImGui::Text("id: %s", node.Id.c_str());
		ImGui::Text("type: %s", node.Type.c_str());
		if (!node.Parent.empty())
			ImGui::Text("parent: %s", node.Parent.c_str());
		if (node.bHasPosition)
			ImGui::Text("pos: %.1f, %.1f", node.X, node.Y);
		if (node.bHasSize)
			ImGui::Text("size: %.1f x %.1f", node.Width, node.Height2D);
		if (node.bClosedPolygon)
			ImGui::Text("polygon: %zu vertices", node.Points.size());
		if (node.BuildingHeight > 0.0f)
			ImGui::Text("height: %.1f", node.BuildingHeight);
		if (node.Floors > 0)
			ImGui::Text("floors: %d", node.Floors);
		for (const WorldLayoutAttribute& attr : node.Attributes)
			ImGui::Text("%s: %s", attr.Key.c_str(), attr.Value.c_str());
	}
	ImGui::EndChild();

	ImGui::SameLine();
	ImGui::BeginChild("##world_layout_canvas", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImVec2 canvasPos = ImGui::GetCursorScreenPos();
	ImVec2 canvasSize = ImGui::GetContentRegionAvail();
	canvasSize.x = std::max(canvasSize.x, 240.0f);
	canvasSize.y = std::max(canvasSize.y, 240.0f);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImVec2 canvasMax(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y);
	draw->AddRectFilled(canvasPos, canvasMax, IM_COL32(11, 13, 17, 255));
	draw->AddRect(canvasPos, canvasMax, IM_COL32(78, 88, 102, 255));

	ImGui::InvisibleButton("##world_layout_canvas_button", canvasSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
	const bool canvasHovered = ImGui::IsItemHovered();
	const bool canvasClicked = canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
	const ImGuiIO& io = ImGui::GetIO();
	if (canvasHovered && io.MouseWheel != 0.0f)
	{
		bWorldLayoutAutoFit = false;
		WorldLayoutZoom = std::clamp(WorldLayoutZoom * (io.MouseWheel > 0.0f ? 1.12f : 0.89f), 0.2f, 4.0f);
	}
	if (canvasHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Right))
	{
		bWorldLayoutAutoFit = false;
		WorldLayoutPanX += io.MouseDelta.x;
		WorldLayoutPanY += io.MouseDelta.y;
	}

	float minX = std::numeric_limits<float>::max();
	float minY = std::numeric_limits<float>::max();
	float maxX = std::numeric_limits<float>::lowest();
	float maxY = std::numeric_limits<float>::lowest();
	auto includePoint = [&](float x, float y)
	{
		minX = std::min(minX, x);
		minY = std::min(minY, y);
		maxX = std::max(maxX, x);
		maxY = std::max(maxY, y);
	};
	for (const WorldLayoutNode& node : LayoutDocument.Nodes)
	{
		if (node.bHasPosition)
		{
			if (node.bHasSize)
			{
				includePoint(node.X - node.Width * 0.5f, node.Y - node.Height2D * 0.5f);
				includePoint(node.X + node.Width * 0.5f, node.Y + node.Height2D * 0.5f);
			}
			else
			{
				includePoint(node.X, node.Y);
			}
		}
		for (const auto& p : node.Points)
			includePoint(p.first, p.second);
	}
	if (minX == std::numeric_limits<float>::max())
	{
		minX = -100.0f; minY = -100.0f; maxX = 100.0f; maxY = 100.0f;
	}
	const float boundsW = std::max(maxX - minX, 1.0f);
	const float boundsH = std::max(maxY - minY, 1.0f);
	const float centerX = (minX + maxX) * 0.5f;
	const float centerY = (minY + maxY) * 0.5f;
	const float fitScale = std::min(canvasSize.x / (boundsW * 1.18f), canvasSize.y / (boundsH * 1.18f));
	const float scale = std::max(0.0001f, fitScale * WorldLayoutZoom);
	if (bWorldLayoutAutoFit)
	{
		WorldLayoutPanX = 0.0f;
		WorldLayoutPanY = 0.0f;
	}
	const ImVec2 canvasCenter(canvasPos.x + canvasSize.x * 0.5f + WorldLayoutPanX, canvasPos.y + canvasSize.y * 0.5f + WorldLayoutPanY);
	auto worldToScreen = [&](float x, float y) -> ImVec2
	{
		return ImVec2(canvasCenter.x + (x - centerX) * scale, canvasCenter.y - (y - centerY) * scale);
	};
	auto screenToWorld = [&](const ImVec2& p) -> std::pair<float, float>
	{
		return {
			centerX + (p.x - canvasCenter.x) / scale,
			centerY - (p.y - canvasCenter.y) / scale
		};
	};
	if ((DraggingWorldLayoutNode >= 0 || DraggingWorldLayoutVertex >= 0) && ImGui::IsMouseDown(ImGuiMouseButton_Left))
	{
		const float dxWorld = io.MouseDelta.x / scale;
		const float dyWorld = -io.MouseDelta.y / scale;
		if (std::abs(dxWorld) > 0.0f || std::abs(dyWorld) > 0.0f)
		{
			if (DraggingWorldLayoutVertex >= 0 &&
				SelectedWorldLayoutNode >= 0 &&
				SelectedWorldLayoutNode < static_cast<int>(LayoutDocument.Nodes.size()) &&
				DraggingWorldLayoutVertex < static_cast<int>(LayoutDocument.Nodes[SelectedWorldLayoutNode].Points.size()))
			{
				auto& p = LayoutDocument.Nodes[SelectedWorldLayoutNode].Points[DraggingWorldLayoutVertex];
				p.first += dxWorld;
				p.second += dyWorld;
			}
			else if (DraggingWorldLayoutNode >= 0 && DraggingWorldLayoutNode < static_cast<int>(LayoutDocument.Nodes.size()))
			{
				MoveWorldLayoutNodeWithChildren(DraggingWorldLayoutNode, dxWorld, dyWorld);
			}
			bWorldLayoutDragMoved = true;
			bWorldLayoutDirty = true;
		}
	}
	if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
	{
		std::string draggedId;
		if (DraggingWorldLayoutVertex >= 0 &&
			SelectedWorldLayoutNode >= 0 &&
			SelectedWorldLayoutNode < static_cast<int>(LayoutDocument.Nodes.size()))
		{
			draggedId = LayoutDocument.Nodes[SelectedWorldLayoutNode].Id;
		}
		else if (DraggingWorldLayoutNode >= 0 && DraggingWorldLayoutNode < static_cast<int>(LayoutDocument.Nodes.size()))
		{
			draggedId = LayoutDocument.Nodes[DraggingWorldLayoutNode].Id;
		}
		if (bWorldLayoutDragMoved && !draggedId.empty())
		{
			const int resolvedIndex = FindWorldLayoutNodeIndex(draggedId);
			bool topologyChanged = ResolveWorldLayoutOverlapsForNode(resolvedIndex);
			topologyChanged = AlignWorldLayoutAdjacentEdgesForNode(FindWorldLayoutNodeIndex(draggedId)) || topologyChanged;
			if (topologyChanged)
				bWorldLayoutDirty = true;
			SelectedWorldLayoutNode = FindWorldLayoutNodeIndex(draggedId);
		}
		DraggingWorldLayoutNode = -1;
		DraggingWorldLayoutVertex = -1;
		bWorldLayoutDragMoved = false;
	}
	auto nodeCenter = [&](const WorldLayoutNode& node) -> ImVec2
	{
		if (!node.Points.empty())
		{
			float sx = 0.0f;
			float sy = 0.0f;
			for (const auto& p : node.Points)
			{
				sx += p.first;
				sy += p.second;
			}
			return worldToScreen(sx / node.Points.size(), sy / node.Points.size());
		}
		return worldToScreen(node.X, node.Y);
	};
	std::unordered_map<std::string, int> nodeIndex;
	for (int i = 0; i < static_cast<int>(LayoutDocument.Nodes.size()); ++i)
		nodeIndex[LayoutDocument.Nodes[i].Id] = i;
	std::vector<int> nodeDepth(LayoutDocument.Nodes.size(), 0);
	for (size_t pass = 0; pass < LayoutDocument.Nodes.size(); ++pass)
	{
		bool changed = false;
		for (int i = 0; i < static_cast<int>(LayoutDocument.Nodes.size()); ++i)
		{
			const std::string& parent = LayoutDocument.Nodes[i].Parent;
			if (parent.empty())
				continue;
			auto parentIt = nodeIndex.find(parent);
			if (parentIt == nodeIndex.end())
				continue;
			const int nextDepth = nodeDepth[parentIt->second] + 1;
			if (nextDepth > nodeDepth[i])
			{
				nodeDepth[i] = nextDepth;
				changed = true;
			}
		}
		if (!changed)
			break;
	}
	auto nodeVisualArea = [](const WorldLayoutNode& node) -> float
	{
		if (node.bClosedPolygon)
			return std::max(1.0f, LayoutPolygonArea(node.Points));
		if (node.bHasSize)
			return std::max(1.0f, node.Width * node.Height2D);
		if (node.Points.size() > 1)
		{
			float length = 0.0f;
			for (size_t k = 1; k < node.Points.size(); ++k)
			{
				const float dx = node.Points[k].first - node.Points[k - 1].first;
				const float dy = node.Points[k].second - node.Points[k - 1].second;
				length += std::sqrt(dx * dx + dy * dy);
			}
			return std::max(1.0f, length * std::max(node.Width, 160.0f));
		}
		return 1.0f;
	};
	auto nodeRenderPolygon = [](const WorldLayoutNode& node, std::vector<std::pair<float, float>>& out) -> bool
	{
		out.clear();
		if (node.bClosedPolygon && node.Points.size() >= 3)
		{
			out = node.Points;
			return true;
		}
		if (node.bHasSize)
		{
			const float x0 = node.X - node.Width * 0.5f;
			const float x1 = node.X + node.Width * 0.5f;
			const float y0 = node.Y - node.Height2D * 0.5f;
			const float y1 = node.Y + node.Height2D * 0.5f;
			out = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
			return true;
		}
		return false;
	};
	auto collectSharedBoundarySegments = [](
		const std::vector<std::pair<float, float>>& a,
		const std::vector<std::pair<float, float>>& b,
		std::vector<std::pair<std::pair<float, float>, std::pair<float, float>>>& out)
	{
		constexpr float kLineTolerance = 10.0f;
		constexpr float kMinSharedLength = 96.0f;
		for (size_t i = 0; i < a.size(); ++i)
		{
			const auto& a0 = a[i];
			const auto& a1 = a[(i + 1) % a.size()];
			float ax = a1.first - a0.first;
			float ay = a1.second - a0.second;
			const float aLen = std::sqrt(ax * ax + ay * ay);
			if (aLen < kMinSharedLength)
				continue;
			ax /= aLen;
			ay /= aLen;
			for (size_t j = 0; j < b.size(); ++j)
			{
				const auto& b0 = b[j];
				const auto& b1 = b[(j + 1) % b.size()];
				const float bx = b1.first - b0.first;
				const float by = b1.second - b0.second;
				const float bLen = std::sqrt(bx * bx + by * by);
				if (bLen < kMinSharedLength)
					continue;
				const float cross = std::abs(ax * by / bLen - ay * bx / bLen);
				if (cross > 0.015f)
					continue;
				const float d0 = std::abs((b0.first - a0.first) * (-ay) + (b0.second - a0.second) * ax);
				const float d1 = std::abs((b1.first - a0.first) * (-ay) + (b1.second - a0.second) * ax);
				if (d0 > kLineTolerance || d1 > kLineTolerance)
					continue;

				float bT0 = (b0.first - a0.first) * ax + (b0.second - a0.second) * ay;
				float bT1 = (b1.first - a0.first) * ax + (b1.second - a0.second) * ay;
				if (bT0 > bT1)
					std::swap(bT0, bT1);
				const float overlap0 = std::max(0.0f, bT0);
				const float overlap1 = std::min(aLen, bT1);
				if (overlap1 - overlap0 < kMinSharedLength)
					continue;
				out.push_back({
					{ a0.first + ax * overlap0, a0.second + ay * overlap0 },
					{ a0.first + ax * overlap1, a0.second + ay * overlap1 }
				});
			}
		}
	};
	std::vector<int> drawOrder;
	drawOrder.reserve(LayoutDocument.Nodes.size());
	for (int i = 0; i < static_cast<int>(LayoutDocument.Nodes.size()); ++i)
		drawOrder.push_back(i);
	std::sort(drawOrder.begin(), drawOrder.end(),
		[&](int a, int b)
		{
			if (nodeDepth[a] != nodeDepth[b])
				return nodeDepth[a] < nodeDepth[b];
			const int layerA = LayoutDrawLayer(LayoutDocument.Nodes[a].Type);
			const int layerB = LayoutDrawLayer(LayoutDocument.Nodes[b].Type);
			if (layerA != layerB)
				return layerA < layerB;
			return nodeVisualArea(LayoutDocument.Nodes[a]) > nodeVisualArea(LayoutDocument.Nodes[b]);
		});

	for (int gx = -8; gx <= 8; ++gx)
	{
		const float t = gx / 8.0f;
		const float x = canvasPos.x + canvasSize.x * (0.5f + t * 0.5f);
		const float y = canvasPos.y + canvasSize.y * (0.5f + t * 0.5f);
		draw->AddLine(ImVec2(x, canvasPos.y), ImVec2(x, canvasMax.y), IM_COL32(255, 255, 255, gx == 0 ? 26 : 11));
		draw->AddLine(ImVec2(canvasPos.x, y), ImVec2(canvasMax.x, y), IM_COL32(255, 255, 255, gx == 0 ? 26 : 11));
	}

	int clickedNode = -1;
	int clickedVertex = -1;
	int clickedEdgeNode = -1;
	int clickedEdgeIndex = -1;
	float clickedEdgeDistanceSq = std::numeric_limits<float>::max();
	std::pair<float, float> clickedEdgeWorld = { 0.0f, 0.0f };
	int clickedDepth = -1;
	float clickedArea = std::numeric_limits<float>::max();
	auto registerHit = [&](int i, float area)
	{
		if (!canvasClicked)
			return;
		const int depth = nodeDepth[i];
		if (depth > clickedDepth || (depth == clickedDepth && area < clickedArea))
		{
			clickedNode = i;
			clickedDepth = depth;
			clickedArea = area;
		}
	};
	auto distanceToSegmentSq = [](const ImVec2& p, const ImVec2& a, const ImVec2& b) -> float
	{
		const float vx = b.x - a.x;
		const float vy = b.y - a.y;
		const float wx = p.x - a.x;
		const float wy = p.y - a.y;
		const float lenSq = vx * vx + vy * vy;
		const float t = lenSq > 1.0e-5f ? std::clamp((wx * vx + wy * vy) / lenSq, 0.0f, 1.0f) : 0.0f;
		const float dx = p.x - (a.x + vx * t);
		const float dy = p.y - (a.y + vy * t);
		return dx * dx + dy * dy;
	};
	auto closestPointOnSegment = [](const ImVec2& p, const ImVec2& a, const ImVec2& b) -> ImVec2
	{
		const float vx = b.x - a.x;
		const float vy = b.y - a.y;
		const float wx = p.x - a.x;
		const float wy = p.y - a.y;
		const float lenSq = vx * vx + vy * vy;
		const float t = lenSq > 1.0e-5f ? std::clamp((wx * vx + wy * vy) / lenSq, 0.0f, 1.0f) : 0.0f;
		return ImVec2(a.x + vx * t, a.y + vy * t);
	};
	for (int drawIndex : drawOrder)
	{
		const int i = drawIndex;
		const WorldLayoutNode& node = LayoutDocument.Nodes[i];
		const bool selected = i == SelectedWorldLayoutNode;
		const ImU32 outline = ColorForLayoutType(node.Type, selected, false);
		const ImU32 fill = ColorForLayoutType(node.Type, selected, true);
		const float visualArea = nodeVisualArea(node);
		const bool overlay = IsLayoutOverlayKind(node.Type);

		if (!node.Points.empty())
		{
			if (node.bClosedPolygon && node.Points.size() >= 3)
			{
				std::vector<ImVec2> screenPoints;
				screenPoints.reserve(node.Points.size());
				for (const auto& p : node.Points)
					screenPoints.push_back(worldToScreen(p.first, p.second));
				if (!overlay || selected)
					draw->AddConcavePolyFilled(screenPoints.data(), static_cast<int>(screenPoints.size()), fill);
				draw->AddPolyline(screenPoints.data(), static_cast<int>(screenPoints.size()), outline, ImDrawFlags_Closed, selected ? 3.0f : (overlay ? 1.2f : 1.7f));
				if (selected)
				{
					for (int v = 0; v < static_cast<int>(screenPoints.size()); ++v)
					{
						const ImVec2 hp = screenPoints[v];
						draw->AddCircleFilled(hp, 5.5f, IM_COL32(255, 235, 136, 245));
						draw->AddCircle(hp, 7.0f, IM_COL32(35, 28, 12, 230), 14, 1.4f);
						if (canvasClicked)
						{
							const float dx = io.MousePos.x - hp.x;
							const float dy = io.MousePos.y - hp.y;
							if (dx * dx + dy * dy <= 96.0f)
							{
								clickedNode = i;
								clickedVertex = v;
							}
						}
					}
				}
				if (selected && canvasClicked && clickedVertex < 0)
				{
					for (int edge = 0; edge < static_cast<int>(screenPoints.size()); ++edge)
					{
						const ImVec2 a = screenPoints[edge];
						const ImVec2 b = screenPoints[(edge + 1) % screenPoints.size()];
						const float distSq = distanceToSegmentSq(io.MousePos, a, b);
						if (distSq > 100.0f || distSq >= clickedEdgeDistanceSq)
							continue;
						clickedEdgeNode = i;
						clickedEdgeIndex = edge;
						clickedEdgeDistanceSq = distSq;
						clickedEdgeWorld = screenToWorld(closestPointOnSegment(io.MousePos, a, b));
					}
				}
				if (canvasClicked && clickedVertex < 0 && clickedEdgeNode < 0 && PointInPolygon(io.MousePos, screenPoints))
					registerHit(i, visualArea);
			}
			else
			{
				const bool road = IsLayoutRoadKind(node.Type);
				const float minThickness = road ? 5.0f : 2.0f;
				const float maxThickness = road ? 56.0f : 22.0f;
				const float routeThickness = std::clamp(std::max(node.Width, 160.0f) * scale, minThickness, maxThickness);
				for (size_t k = 1; k < node.Points.size(); ++k)
				{
					const ImVec2 a = worldToScreen(node.Points[k - 1].first, node.Points[k - 1].second);
					const ImVec2 b = worldToScreen(node.Points[k].first, node.Points[k].second);
					if (road)
					{
						const ImU32 roadEdge = selected ? IM_COL32(255, 226, 122, 245) : IM_COL32(24, 28, 32, 245);
						const ImU32 roadFill = node.Type == "alley" ? IM_COL32(70, 74, 78, 235) : IM_COL32(58, 63, 70, 245);
						draw->AddLine(a, b, roadEdge, routeThickness + 7.0f);
						draw->AddLine(a, b, roadFill, routeThickness);
						if (routeThickness >= 12.0f)
							draw->AddLine(a, b, IM_COL32(225, 205, 142, 130), std::max(1.0f, routeThickness * 0.08f));
					}
					else
					{
						draw->AddLine(a, b, IM_COL32(18, 20, 24, 190), routeThickness + 3.0f);
						draw->AddLine(a, b, outline, routeThickness);
					}
					if (canvasClicked && distanceToSegmentSq(io.MousePos, a, b) <= (routeThickness + 6.0f) * (routeThickness + 6.0f))
						registerHit(i, visualArea);
				}
			}
			const ImVec2 c = nodeCenter(node);
			draw->AddCircleFilled(c, selected ? 6.0f : 4.0f, IsLayoutRoadKind(node.Type) ? IM_COL32(235, 220, 160, 230) : outline);
			if (canvasClicked)
			{
				const float dx = io.MousePos.x - c.x;
				const float dy = io.MousePos.y - c.y;
				if ((dx * dx + dy * dy) < 144.0f)
					registerHit(i, visualArea);
			}
		}
		else if (node.bHasSize)
		{
			const ImVec2 a = worldToScreen(node.X - node.Width * 0.5f, node.Y + node.Height2D * 0.5f);
			const ImVec2 b = worldToScreen(node.X + node.Width * 0.5f, node.Y - node.Height2D * 0.5f);
			if (!overlay || selected)
				draw->AddRectFilled(a, b, fill, 2.0f);
			draw->AddRect(a, b, outline, 2.0f, 0, selected ? 3.0f : (overlay ? 1.2f : 1.6f));
			if ((node.Type == "building" || node.Type == "landmark") && node.Floors > 0)
			{
				const float floorStep = std::max(4.0f, std::abs(b.y - a.y) / std::max(node.Floors, 1));
				for (int f = 1; f < node.Floors && f < 20; ++f)
				{
					const float y = a.y + floorStep * f;
					draw->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), IM_COL32(255, 255, 255, 34), 1.0f);
				}
			}
			if (selected)
			{
				const std::pair<float, float> corners[4] = {
					{ node.X - node.Width * 0.5f, node.Y - node.Height2D * 0.5f },
					{ node.X + node.Width * 0.5f, node.Y - node.Height2D * 0.5f },
					{ node.X + node.Width * 0.5f, node.Y + node.Height2D * 0.5f },
					{ node.X - node.Width * 0.5f, node.Y + node.Height2D * 0.5f }
				};
				for (int v = 0; v < 4; ++v)
				{
					const ImVec2 hp = worldToScreen(corners[v].first, corners[v].second);
					draw->AddCircleFilled(hp, 5.5f, IM_COL32(255, 235, 136, 245));
					draw->AddCircle(hp, 7.0f, IM_COL32(35, 28, 12, 230), 14, 1.4f);
					if (canvasClicked)
					{
						const float dx = io.MousePos.x - hp.x;
						const float dy = io.MousePos.y - hp.y;
						if (dx * dx + dy * dy <= 96.0f)
						{
							clickedNode = i;
							clickedVertex = v;
						}
					}
				}
				if (canvasClicked && clickedVertex < 0)
				{
					for (int edge = 0; edge < 4; ++edge)
					{
						const ImVec2 a = worldToScreen(corners[edge].first, corners[edge].second);
						const ImVec2 b = worldToScreen(corners[(edge + 1) % 4].first, corners[(edge + 1) % 4].second);
						const float distSq = distanceToSegmentSq(io.MousePos, a, b);
						if (distSq > 100.0f || distSq >= clickedEdgeDistanceSq)
							continue;
						clickedEdgeNode = i;
						clickedEdgeIndex = edge;
						clickedEdgeDistanceSq = distSq;
						clickedEdgeWorld = screenToWorld(closestPointOnSegment(io.MousePos, a, b));
					}
				}
			}
			if (canvasClicked && io.MousePos.x >= std::min(a.x, b.x) && io.MousePos.x <= std::max(a.x, b.x) &&
				io.MousePos.y >= std::min(a.y, b.y) && io.MousePos.y <= std::max(a.y, b.y))
			{
				registerHit(i, visualArea);
			}
		}
		else if (node.bHasPosition)
		{
			const ImVec2 c = worldToScreen(node.X, node.Y);
			draw->AddCircleFilled(c, selected ? 7.0f : 5.0f, fill);
			draw->AddCircle(c, outline, selected ? 8.0f : 6.0f, 16, selected ? 2.5f : 1.5f);
			if (canvasClicked)
			{
				const float dx = io.MousePos.x - c.x;
				const float dy = io.MousePos.y - c.y;
				if ((dx * dx + dy * dy) < 144.0f)
					registerHit(i, visualArea);
			}
		}

		const ImVec2 labelPos = nodeCenter(node);
		if ((!overlay && scale > 0.008f) || selected)
			draw->AddText(ImVec2(labelPos.x + 5.0f, labelPos.y - 8.0f), selected ? IM_COL32(255, 238, 158, 255) : IM_COL32(222, 229, 235, 210), node.Label.c_str());
	}
	if (bWorldLayoutShowImplicitRoads)
	{
		std::vector<std::pair<std::pair<float, float>, std::pair<float, float>>> implicitRoads;
		for (int aIndex = 0; aIndex < static_cast<int>(LayoutDocument.Nodes.size()); ++aIndex)
		{
			const WorldLayoutNode& aNode = LayoutDocument.Nodes[aIndex];
			if (!IsLayoutPartitionKind(aNode.Type))
				continue;
			std::vector<std::pair<float, float>> aPoly;
			if (!nodeRenderPolygon(aNode, aPoly))
				continue;
			for (int bIndex = aIndex + 1; bIndex < static_cast<int>(LayoutDocument.Nodes.size()); ++bIndex)
			{
				const WorldLayoutNode& bNode = LayoutDocument.Nodes[bIndex];
				if (aNode.Parent != bNode.Parent || !IsLayoutPartitionKind(bNode.Type))
					continue;
				std::vector<std::pair<float, float>> bPoly;
				if (!nodeRenderPolygon(bNode, bPoly))
					continue;
				collectSharedBoundarySegments(aPoly, bPoly, implicitRoads);
			}
		}
		const float roadThickness = std::clamp(420.0f * scale, 5.0f, 28.0f);
		for (const auto& segment : implicitRoads)
		{
			const ImVec2 a = worldToScreen(segment.first.first, segment.first.second);
			const ImVec2 b = worldToScreen(segment.second.first, segment.second.second);
			draw->AddLine(a, b, IM_COL32(18, 20, 24, 240), roadThickness + 6.0f);
			draw->AddLine(a, b, IM_COL32(62, 67, 74, 245), roadThickness);
			if (roadThickness >= 12.0f)
				draw->AddLine(a, b, IM_COL32(225, 205, 142, 120), std::max(1.0f, roadThickness * 0.075f));
		}
	}
	if (clickedEdgeNode >= 0 && clickedEdgeIndex >= 0)
	{
		SelectedWorldLayoutNode = clickedEdgeNode;
		WorldLayoutNode& node = LayoutDocument.Nodes[SelectedWorldLayoutNode];
		if (node.Points.empty() && node.bHasSize)
		{
			node.Points = {
				{ node.X - node.Width * 0.5f, node.Y - node.Height2D * 0.5f },
				{ node.X + node.Width * 0.5f, node.Y - node.Height2D * 0.5f },
				{ node.X + node.Width * 0.5f, node.Y + node.Height2D * 0.5f },
				{ node.X - node.Width * 0.5f, node.Y + node.Height2D * 0.5f }
			};
			node.bClosedPolygon = true;
			node.bHasPosition = false;
			node.bHasSize = false;
		}
		if (node.Points.size() >= 2)
		{
			const int insertIndex = std::clamp(clickedEdgeIndex + 1, 0, static_cast<int>(node.Points.size()));
			node.Points.insert(node.Points.begin() + insertIndex, clickedEdgeWorld);
			DraggingWorldLayoutVertex = insertIndex;
			DraggingWorldLayoutNode = -1;
			bWorldLayoutDragMoved = false;
			bWorldLayoutDirty = true;
		}
	}
	else if (clickedVertex >= 0 && clickedNode >= 0)
	{
		SelectedWorldLayoutNode = clickedNode;
		WorldLayoutNode& node = LayoutDocument.Nodes[SelectedWorldLayoutNode];
		if (node.Points.empty() && node.bHasSize)
		{
			node.Points = {
				{ node.X - node.Width * 0.5f, node.Y - node.Height2D * 0.5f },
				{ node.X + node.Width * 0.5f, node.Y - node.Height2D * 0.5f },
				{ node.X + node.Width * 0.5f, node.Y + node.Height2D * 0.5f },
				{ node.X - node.Width * 0.5f, node.Y + node.Height2D * 0.5f }
			};
			node.bClosedPolygon = true;
			node.bHasPosition = false;
			node.bHasSize = false;
			bWorldLayoutDirty = true;
		}
		DraggingWorldLayoutVertex = clickedVertex;
		DraggingWorldLayoutNode = -1;
		bWorldLayoutDragMoved = false;
	}
	else if (clickedNode >= 0)
	{
		SelectedWorldLayoutNode = clickedNode;
		DraggingWorldLayoutNode = clickedNode;
		DraggingWorldLayoutVertex = -1;
		bWorldLayoutDragMoved = false;
	}

	draw->AddText(ImVec2(canvasPos.x + 10.0f, canvasMax.y - 24.0f), IM_COL32(180, 190, 202, 190),
		"Mouse wheel: zoom / Right-drag: pan / Left-drag: edit");
	ImGui::EndChild();
	ImGui::End();
}

void CoronaAssetExplorer::DrawBottomToggleButton()
{
	const ImGuiViewport* vp = ImGui::GetMainViewport();
	const float bw = 110.0f;
	// Sit to the right of the scene inspector's toggle button so both fit at
	// the bottom edge without overlap.
	const float x = vp->WorkPos.x + 6.0f + 110.0f + 6.0f;
	ImGui::SetNextWindowPos(ImVec2(x, vp->WorkPos.y + vp->WorkSize.y - kToggleBtnH - 6.0f));
	ImGui::SetNextWindowSize(ImVec2(bw, kToggleBtnH));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(2.0f, 2.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
	const ImGuiWindowFlags flags =
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar;
	ImGui::Begin("##asset_explorer_toggle", nullptr, flags);
	if (ImGui::Button(bVisible ? "Hide Assets" : "Show Assets", ImVec2(-1, -1)))
		bVisible = !bVisible;
	ImGui::End();
	ImGui::PopStyleVar(2);
}
