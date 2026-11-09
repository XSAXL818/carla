# UserContent

Project-owned Unreal assets live here and are tracked by this repository.

Official CARLA assets under `../Carla` come from the separate
`carla-simulator/carla-content` repository and are intentionally ignored.
After a fresh clone, restore them with:

```powershell
git clone -b ue58-dev-carla https://bitbucket.org/carla-simulator/carla-content.git Content/Carla
```

Prefer small assets here. Large maps and imported meshes should be stored
outside Git or in a dedicated LFS repository.
