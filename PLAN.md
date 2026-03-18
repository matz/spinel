# PLAN: Spinel AOT Compiler

Ruby source → Prism AST → whole-program type inference → standalone C executable.
No runtime dependencies (no mruby, no GC library — GC is generated inline).

詳細設計は `ruby_aot_compiler_design.md` を参照。

---

## 現状 (Status)

### 完了した機能

**コンパイラアーキテクチャ** (~4000行のC)
- Prism (libprism) によるRubyパース
- 多パスコード生成:
  1. クラス/モジュール/関数解析
  2. 全変数・パラメータ・戻り値の型推論
  3. C構造体・メソッド関数の生成
  4. main()のトップレベルコード生成
- マーク&スイープGC (シャドウスタックによるルート管理)

**サポート済み言語機能**

| カテゴリ | 機能 |
|---------|------|
| オブジェクト | クラス定義、インスタンス変数、メソッド定義、getter/setter自動インライン化 |
| | コンストラクタ (`.new`)、型付きオブジェクトへのメソッド呼び出し |
| | モジュール (状態変数 + メソッド) |
| 型 | Integer, Float, Boolean, String, nil → アンボックスCの型 |
| | 値型 (Vec: 3 floats → 値渡し) vs ポインタ型 |
| 配列 | sp_IntArray (push/pop/shift/dup/reverse!/empty?/length/[]/!=) |
| | O(1) shift (デキュー方式のstartオフセット) |
| クロージャ | `-> x { body }` ラムダ → Cクロージャ (キャプチャ解析) |
| | sp_Val タグ付きユニオン (Proc/Int/Bool/Nil) |
| | アリーナアロケータ (mmap、デマンドページング) |
| 制御 | while, if/elsif/else, ternary, until |
| | break, return, and/or/not |
| | Integer#times with block → C forループ |
| 演算 | 算術 (+, -, *, /, %), 比較, ビット演算 → 直接C演算子 |
| | 単項マイナス, 複合代入 (+=, <<=) |
| | Math.sqrt/cos/sin → C math関数 |
| I/O | puts, print, printf, putc, p → stdio |
| | 文字列補間 → printf |
| | Integer#chr → putchar |
| その他 | 並列代入, チェーン代入 (zr = zi = 0) |
| | 定数 (グローバルスコープ), トップレベル関数 |
| | 関数間型推論 (呼び出しサイトからのパラメータ型推論) |
| GC | マーク&スイープ (非値型オブジェクト・配列用) |
| | シャドウスタックルート管理, ファイナライザ |
| | GC不要なプログラムではGCコード省略 |

### ベンチマーク結果

| ベンチマーク | CRuby | mruby | Spinel AOT | 高速化 | メモリ |
|-------------|-------|-------|------------|--------|--------|
| mandelbrot (600×600) | 1.14s | 3.18s | 0.02s | 57× | <1MB |
| ao_render (64×64 AO) | 3.55s | 13.69s | 0.07s | 51× | 2MB |
| so_lists (300×10K) | 0.44s | 2.01s | 0.02s | 22× | 2MB |
| fib(34) | 0.55s | 2.78s | 0.01s | 55× | <1MB |
| lc_fizzbuzz (Church) | 28.96s | — | 1.55s | 19× | arena |
| mandel_term | 0.05s | 0.05s | ~0s | 50×+ | <1MB |

生成バイナリは完全スタンドアロン (libc + libm のみ、mruby不要)。

---

## 未サポート機能

### 高優先度 (次のターゲット候補)

| 機能 | 必要なベンチマーク例 | 難易度 |
|------|-------------------|--------|
| 継承 (`class Dog < Animal`) | OOP系ベンチマーク | 中 |
| `yield` / 暗黙ブロック | each, map, select等 | 中 |
| `case`/`when` | パターンマッチ系 | 低 |
| `unless` | 一般的なRubyコード | 低 |
| 例外処理 (`rescue`/`raise`) | エラーハンドリング系 | 中 |
| Hash リテラル・操作 | 多くの実用プログラム | 中 |
| Symbol | メソッド名、キー | 低〜中 |
| `each` / `map` / `select` (配列) | Enumerable系 | 中 |
| デフォルト引数 | `def foo(x = 10)` | 低 |
| `super` | 継承チェーン | 中 |

### 中優先度

| 機能 | 備考 |
|------|------|
| String メソッド (gsub, split, match等) | 文字列処理 |
| Regexp | パターンマッチ |
| キーワード引数 | `def foo(name:, age:)` |
| スプラット (`*args`, `**kwargs`) | 可変長引数 |
| `attr_accessor` / `attr_reader` マクロ | 手動getter/setterは対応済み |
| `include` / `extend` | Mixin |
| `Comparable`, `Enumerable` | モジュール組み込み |
| `for..in` + Range | while版は対応済み |
| `loop do` | while(1)で代替可 |
| `next` (ループ内) | continue相当 |

### 低優先度 (動的機能)

| 機能 | 備考 |
|------|------|
| `eval`, `instance_eval` | 静的解析不可 |
| `send`, `public_send` | 動的ディスパッチ |
| `define_method` | 動的メソッド定義 |
| `method_missing` | フォールバック |
| `require`, `load` | モジュールシステム |
| File I/O | OS依存 |
| グローバル変数 (`$stdout`等) | ランタイム依存 |
| クラス変数 (`@@var`) | 使用頻度低 |
| open class / monkey patching | 静的解析と相性悪 |

---

## アーキテクチャ

```
Ruby Source (.rb)
    |
    v
Prism (libprism)                -- パース → AST
    |
    v
Pass 1: クラス解析              -- クラス、メソッド、インスタンス変数の検出
    |
    v
Pass 2: 型推論                  -- 全変数・ivar・パラメータの型推論
    |                              (Integer/Float/Boolean/Object/Array/Proc)
    |                              関数間型推論 (呼び出しサイト解析)
    v
Pass 3: 構造体・メソッド生成    -- クラス → C構造体
    |                              メソッド → C関数 (直接呼び出し)
    |                              getter/setter → インラインフィールドアクセス
    |                              GCスキャン関数生成
    v
Pass 4: main() コード生成       -- トップレベルコード → main()
    |                              while/for/times → Cループ
    |                              算術 → C演算子
    |                              puts/print/printf → stdio
    v
スタンドアロンCファイル
    |
    v
cc -O2 -lm → ネイティブバイナリ  -- mruby不要、GC内蔵、libc+libmのみ
```

## ビルドフロー

```bash
# コンパイラのビルド
make deps   # Prismを取得・ビルド
make        # spinelコンパイラをビルド

# Rubyプログラムのコンパイル
./spinel --source=examples/bm_fib.rb --output=fib.c
cc -O2 fib.c -lm -o fib
./fib   # → 5702887

# テスト
make test   # mandelbrotをコンパイル・実行・CRubyと出力比較
```

## プロジェクト構成

```
spinel/
├── src/
│   ├── main.c          # CLI、ファイル読み込み、Prismパース
│   ├── codegen.h       # 型システム、クラス/メソッド/モジュール情報構造体
│   └── codegen.c       # 多パスコード生成器 (~4000行)
├── examples/
│   ├── bm_so_mandelbrot.rb   # Mandelbrot集合
│   ├── bm_ao_render.rb       # AOレイトレーサー (6クラス)
│   ├── bm_so_lists.rb        # 配列操作
│   ├── bm_fib.rb             # 再帰フィボナッチ
│   ├── bm_app_lc_fizzbuzz.rb # λ計算FizzBuzz (1201ラムダ)
│   └── bm_mandel_term.rb     # ターミナルMandelbrot
├── prototype/
│   └── tools/          # Step 0プロトタイプ (RBS抽出、LumiTrace等)
├── Makefile
├── PLAN.md             # 本文書
└── ruby_aot_compiler_design.md  # 詳細設計文書
```

## 次のステップ

1. **継承サポート** — vtable不要のCHA証明済み直接呼び出し
2. **yield/ブロック** — Integer#times以外のイテレータ対応
3. **case/when** — Cのswitch文へ変換
4. **例外処理** — setjmp/longjmpベースのrescue/raise
5. **Hash** — 組み込みハッシュテーブル実装
6. **LumiTraceプロファイル統合** — 型推論の精度向上

## 参考情報

- 詳細設計: `ruby_aot_compiler_design.md`
- プロトタイプツール: `prototype/`
