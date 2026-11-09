# 基于 CARLA 的开发项目

本项目基于 [CARLA Simulator](https://github.com/carla-simulator/carla) 的
`ue58-dev` 分支开发，并在其基础上增加新的内容与功能。

## 分支说明

| 分支 | 用途 |
|---|---|
| `ue58-dev` | 官方 CARLA UE 5.8 基线，保持干净，用于同步上游更新 |
| `dev` | 本项目开发分支，所有新增内容和修改都提交在这里 |

## 目录说明

- `Unreal/CarlaUnreal/Content/UserContent/`：项目自有的蓝图、地图、材质等 Unreal 资产。
- `Unreal/CarlaUnreal/Content/Carla/`：官方 CARLA 资产，来自独立的
  `carla-content` 仓库，不纳入本仓库。
- `Unreal/CarlaUnreal/Source/`、`Unreal/CarlaUnreal/Plugins/`：C++ 源码和插件。

## 同步官方更新

```powershell
git switch ue58-dev
git pull --ff-only
git push origin ue58-dev:ue58-dev

git switch dev
git merge ue58-dev
git push
```

如果合并时 `README.md` 冲突，保留本仓库的版本：

```powershell
git checkout --ours README.md
git add README.md
git commit
```

## 参考

- CARLA 官方文档：https://carla.readthedocs.io/
- CARLA 官方仓库：https://github.com/carla-simulator/carla
