---
displayed_sidebar: docs
---

# array_flatten

## 説明

ネストされた配列の一層をフラット化します。

## 構文

```Haskell
array_flatten(param)
```

## パラメータ

`param`: フラット化が必要なネストされた配列。ネストされた配列のみがサポートされており、マルチレベルのネストされた配列も可能です。配列の要素は StarRocks がサポートする任意のデータ型で構いません。

## 戻り値

戻り値のデータ型は、一層フラット化された後の配列型です。

## 例

例 1: 2 レベルのネストされた配列をフラット化します。

```plaintext
mysql> SELECT array_flatten([[1, 2], [1, 4]]) as res;
+-----------+
| res       |
+-----------+
| [1,2,1,4] |
+-----------+
```

例 2: 3 レベルのネストされた配列をフラット化します。

```plaintext
mysql> SELECT array_flatten([[[1],[2]], [[3],[4]]]) as res;
+-------------------+
| res               |
+-------------------+
| [[1],[2],[3],[4]] |
+-------------------+
```