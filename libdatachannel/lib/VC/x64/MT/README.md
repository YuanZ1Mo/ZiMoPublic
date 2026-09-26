# datachannel.lib 分卷使用说明

## 背景

`datachannel.lib` 原始文件 **123.09 MB**,超过 GitHub 单个文件 **100 MB 硬限制**,无法直接推送
(`pre-receive hook` 会拒绝整个 push)。因此拆分为 2 个 `<100MB` 分卷提交,使用时合并还原。

- 原始库文件:`datachannel.lib`(129,071,078 字节 / 123.09 MB)
- 分卷文件:`datachannel.lib.part01 / part02`(90.00 / 33.09 MB,**提交进 git 的是这些**)
- 合并结果与拆分前**逐字节一致**(SHA256 校验,见下)

同目录的 `juice.lib`(1.58MB)、`srtp2.lib`(1.30MB)、`usrsctp.lib`(5.23MB)都在限制以内,
直接提交原始文件即可,不需要拆分。

原始文件的指纹(合并后可用它校验):

```
SHA256 : 5C81161CAEAA999F0639182BA48FCCE9DF511F521CED6F52F1E75BB0ED77D69B
Bytes  : 129071078
```

***

## 1. 首次使用(拆分并提交)

在本目录下执行拆分脚本(生成 2 个分卷,不动原始文件):

```powershell
powershell -ExecutionPolicy Bypass -File .\datachannel_lib_split.ps1
```

输出示例:

```
datachannel.lib.part01  90.00 MB
datachannel.lib.part02  33.09 MB

done: 123.09 MB -> 2 parts
merge with datachannel_lib_merge.ps1
```

**提交时注意:只提交分卷,不要提交原始 datachannel.lib**

```bash
git add libdatachannel/lib/VC/x64/MT/datachannel.lib.part*
git add libdatachannel/lib/VC/x64/MT/datachannel_lib_split.ps1 libdatachannel/lib/VC/x64/MT/datachannel_lib_merge.ps1
git add libdatachannel/lib/VC/x64/MT/README.md
# 原始 .lib 曾经入库,必须先取消跟踪,否则每次提交都会带上 123MB 的大对象
git rm --cached libdatachannel/lib/VC/x64/MT/datachannel.lib
git commit -m "chore: datachannel.lib 拆分为分卷以符合 GitHub 100MB 限制"
```

> ⚠️ **历史重写警告**:`datachannel.lib` 已经在提交历史里存在过
> (提交 `feat: 增加libdatachannel`),GitHub 检查的是**整个历史**而不只是最新提交,
> 所以仅"删文件 + 加分卷"仍然会被拒绝 push。必须先重写历史清掉那个大对象,
> 见文末"附:历史重写"。

***

## 2. 克隆后使用(合并还原)

克隆仓库后,本目录下只有分卷,需要合并还原出原始 `datachannel.lib`:

```powershell
powershell -ExecutionPolicy Bypass -File .\datachannel_lib_merge.ps1
```

输出示例:

```
merged: datachannel.lib.part01
merged: datachannel.lib.part02

done: 2 parts -> datachannel.lib (123.09 MB)
verify: size OK
```

合并完成后 `datachannel.lib` 即还原,可直接链接使用(ZiMoService 的 `AdditionalDependencies`
里已经引用了 `datachannel.lib`)。需要进一步核对时,用背景一节的 SHA256 对比即可。

***

## 3. 更新 datachannel.lib 后重新拆分

库文件升级后,先删掉旧分卷再重新拆分(脚本不自动清理旧分卷):

```powershell
Remove-Item .\datachannel.lib.part*
powershell -ExecutionPolicy Bypass -File .\datachannel_lib_split.ps1
```

再提交新分卷,并把本文件"背景"一节的大小与 SHA256 更新成新值。

***

## 4. 手工合并(备选)

不用脚本时可用系统命令拼接:

```bash
copy /b datachannel.lib.part01 + datachannel.lib.part02 datachannel.lib
```

> 推荐用脚本合并,自带 size 校验,避免 shell 通配符/编码问题。

***

## 附:历史重写(推送被拒时需要)

若 push 被 `remote rejected ... large files detected` 拒绝,说明历史里有大对象。
以移除历史中所有 `libdatachannel/lib/VC/x64/MT/datachannel.lib` 为例
(`git filter-repo` 需先安装):

```bash
# 方法一:git filter-repo(推荐,需 pip install git-filter-repo)
git filter-repo --path libdatachannel/lib/VC/x64/MT/datachannel.lib --invert-paths

# 方法二:git filter-branch(内置,较慢)
git filter-branch --index-filter \
  "git rm --cached --ignore-unmatch libdatachannel/lib/VC/x64/MT/datachannel.lib" -- --all
```

重写历史会改变所有提交哈希,若远程已有历史需 `git push --force`。
**若这是首次推送且远程 main 从未成功,force push 无覆盖风险。**

***

## 文件清单

| 文件                                | 说明                     |
| --------------------------------- | ---------------------- |
| `datachannel.lib`                 | 原始静态库(本地/合并产物,**不入库**) |
| `datachannel.lib.part01~02`       | 分卷(入库)                 |
| `datachannel_lib_split.ps1`       | 拆分脚本                   |
| `datachannel_lib_merge.ps1`       | 合并脚本(带校验)              |
