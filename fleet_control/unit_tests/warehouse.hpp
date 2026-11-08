#ifndef FLEETMANAGERSYSTEM_WIREHOUSE_HPP
#define FLEETMANAGERSYSTEM_WIREHOUSE_HPP

#pragma once

#include <nlohmann/json.hpp>

// Синтетический склад для бенчмарков и тестов — документ карты, как из редактора UI.
// Не часть fms: собирается только с FMS_BUILD_TESTS.
//
//   st0 ─ a0_0 ──→ a0_1 ──→ … ──→ a0_N      проход: узлы через step м, одностороннее движение,
//           │                       │       соседние проходы — в разные стороны
//         a1_0 ←── a1_1 ←── … ←── a1_N      поперечный проход: в обе стороны, в начале, в конце
//           │                       │       и через каждые cross_every узлов
//         a2_0 ─── a2_1 ─── … ─── a2_N      каждый two_way_every-й проход — двусторонний
//
// Узел aA_I — проход A, позиция I. В документе узлы идут по проходам подряд,
// поэтому номер узла в Graph = A * nodes_per_aisle + I; станции — после всех узлов.
struct WarehouseOptions {
    int aisles = 20;            // проходов
    int nodes_per_aisle = 50;   // узлов в проходе
    double step = 1.5;          // м между узлами вдоль прохода
    double aisle_gap = 4.0;     // м между проходами — там стеллаж
    int cross_every = 10;       // поперечный проход через каждые N узлов
    int two_way_every = 4;      // каждый N-й проход двусторонний
    int stations = 10;          // станций слева, у начала проходов
};

nlohmann::json generate_warehouse(const WarehouseOptions& options);

#endif //FLEETMANAGERSYSTEM_WIREHOUSE_HPP
