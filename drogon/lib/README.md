# drogon.lib 分卷使用说明

## 背景

`drogon.lib` 原始文件 **266.87 MB**,超过 GitHub 单个文件 **100 MB 硬限制**,无法直接推送到仓库
(`pre-receive hook` 会拒绝整个 push)。因此拆分为 3 个 `<100MB` 分卷提交,使用时合并还原。

- 原始库文件:`drogon.lib`(266.87 MB)

- 分卷文件:`drogon.lib.part01 / part02 / part03`(90 / 90 / 86.87 MB,**提交进 git 的是这些**)

- 合并结果与拆分前**逐字节一致**(经脚本 size 校验)

***

## 1. 首次使用(拆分并提交)

在 `drogon\lib` 目录下执行拆分脚本(生成 3 个分卷,不动原始文件):

```powershell
powershell -ExecutionPolicy Bypass -File .\drogon_lib_split.ps1
```

输出示例:

```
drogon.lib.part01  90.00 MB
drogon.lib.part02  90.00 MB
drogon.lib.part03  86.87 MB
done: 266.87 MB -> 3 parts
```

**提交时注意:只提交分卷,不要提交原始 drogon.lib**

```bash
git add drogon/lib/drogon.lib.part*
git add drogon/lib/drogon_lib_split.ps1 drogon/lib/drogon_lib_merge.ps1
git commit -m "chore: drogon.lib 拆分为分卷以符合 GitHub 100MB 限制"
```

> ⚠️ **历史重写警告**:如果仓库历史里已经存在过 267MB 的 drogon.lib 对象,
> GitHub 仍会因历史中的大对象拒绝 push(限制检查的是整个历史,不止最新提交)。
> 必须先重写历史清掉旧对象(如 `git filter-repo --path drogon/lib/drogon.lib --invert-paths`
> 或 `git filter-branch`),再 push。参见文末"附:历史重写"。

***

## 2. 克隆后使用(合并还原)

克隆仓库后,`drogon\lib` 下只有分卷,需要合并还原出原始 `drogon.lib`:

```powershell
powershell -ExecutionPolicy Bypass -File .\drogon_lib_merge.ps1
```

输出示例:

```
merged: drogon.lib.part01
merged: drogon.lib.part02
merged: drogon.lib.part03
done: 3 parts -> drogon.lib (266.87 MB)
verify: size OK
```

合并完成后 `drogon.lib` 即还原,可直接链接使用。

***

## 3. 更新 drogon.lib 后重新拆分

库文件升级后,先删掉旧分卷再重新拆分(脚本不自动清理旧分卷):

```powershell
Remove-Item .\drogon.lib.part*
powershell -ExecutionPolicy Bypass -File .\drogon_lib_split.ps1
```

再提交新分卷。

***

## 4. 手工合并(备选)

不用脚本时可用系统命令拼接:

```bash
copy /b drogon.lib.part01 + drogon.lib.part02 + drogon.lib.part03 drogon.lib
```

> 推荐用脚本合并,自带 size 校验,避免 shell 通配符/编码问题。

***

## 附:历史重写(仅首次推送遇到拒绝时需要)

若 push 被 `remote rejected ... large files detected` 拒绝,说明历史里有大对象。
以移除历史中所有 `drogon/lib/drogon.lib` 为例(`git filter-repo` 需先安装):

```bash
# 方法一:git filter-repo(推荐,需 pip install git-filter-repo)
git filter-repo --path drogon/lib/drogon.lib --invert-paths

# 方法二:git filter-branch(内置,较慢)
git filter-branch --index-filter \
  "git rm --cached --ignore-unmatch drogon/lib/drogon.lib" -- --all
```

重写历史会改变所有提交哈希,若远程已有历史需 `git push --force`。
**若这是首次推送且远程 main 从未成功,force push 无覆盖风险。**

***

## 文件清单

| 文件                     | 说明                     |
| ---------------------- | ---------------------- |
| `drogon.lib`           | 原始静态库(本地/合并产物,**不入库**) |
| `drogon.lib.part01~03` | 分卷(入库)                 |
| `drogon_lib_split.ps1` | 拆分脚本                   |
| `drogon_lib_merge.ps1` | 合并脚本(带校验)              |

