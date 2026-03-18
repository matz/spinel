# PLAN: Spinel AOT Compiler

Ruby source → Prism AST → whole-program type inference → standalone C executable.
No runtime dependencies (no mruby, no GC library — GC is generated inline).
Regexp対応プログラムのみ libonig をリンク。

詳細設計は `ruby_aot_compiler_design.md` を参照。

---

## 現状 (Status)

### コンパイラアーキテクチャ (~7400行のC)

- Prism (libprism) によるRubyパース
- 多パスコード生成:
  1. クラス/モジュール/関数解析 (継承チェーン、mixin解決、Struct.new展開含む)
  2. 全変数・パラメータ・戻り値の型推論 (関数間解析)
  3. C構造体・メソッド関数の生成 (GCスキャン関数含む)
  4. ラムダ/クロージャのキャプチャ解析・コード生成
  5. yield/ブロックのコールバック関数生成
  6. 正規表現パターンのプリコンパイル (oniguruma)
  7. main()のトップレベルコード生成
- マーク&スイープGC (シャドウスタック、ファイナライザ)
- setjmp/longjmpベース例外処理
- アリーナアロケータ (ラムダ/クロージャ用)

### サポート済み言語機能

| カテゴリ | 機能 |
|---------|------|
| **OOP** | クラス定義、インスタンス変数、メソッド定義 |
| | 継承 (`class Dog < Animal`)、`super` |
| | `include` (mixin) — モジュールのインスタンスメソッド取り込み |
| | `attr_accessor` / `attr_reader` / `attr_writer` |
| | クラスメソッド (`def self.foo`) |
| | `Struct.new(:x, :y)` — 合成クラス生成 |
| | getter/setter自動インライン化 |
| | コンストラクタ (`.new`)、型付きオブジェクトへのメソッド呼び出し |
| | モジュール (状態変数 + メソッド) |
| **イントロスペクション** | `is_a?` — 継承チェーンをコンパイル時に静的解決 |
| | `respond_to?` — メソッドテーブルをコンパイル時に静的解決 |
| | `nil?` — nil以外は常にFALSE |
| **ブロック/クロージャ** | `yield`、ブロック付きメソッド呼び出し (キャプチャ変数) |
| | `Array#each/map/select/reject` (インライン化) |
| | `Hash#each` (キー/値ペア) |
| | `Integer#times/upto/downto` with block → C forループ |
| | `-> x { body }` ラムダ → Cクロージャ (キャプチャ解析) |
| | sp_Val タグ付きユニオン + アリーナアロケータ |
| **制御** | while, until, if/elsif/else, unless |
| | case/when/else (値、複数値、Range条件) |
| | for..in + Range, loop do |
| | break, next, return |
| | ternary, and/or/not |
| **例外処理** | begin/rescue/ensure/retry |
| | raise "message" (setjmp/longjmp) |
| | rescue => e (メッセージキャプチャ) |
| | volatile変数でlongjmpの値保存 |
| **引数** | 位置引数、デフォルト値 (`def foo(x = 10)`) |
| | キーワード引数 (`def foo(name:, greeting: "Hello")`) |
| | 可変長引数/スプラット (`def sum(*nums)`) |
| **型** | Integer, Float, Boolean, String, Symbol, nil → アンボックスC型 |
| | 値型 (Vec: 3 floats → 値渡し) vs ポインタ型 |
| **コレクション** | sp_IntArray (push/pop/shift/dup/reverse!/each/map/select/reject) |
| | Array#first/last/include?/length |
| | sp_StrIntHash (文字列キー→整数値、each/has_key?/delete) |
| | sp_StrArray (文字列配列、split結果用) |
| | O(1) shift (デキュー方式のstartオフセット) |
| **正規表現** | `/pattern/` リテラル → onigurumaプリコンパイル |
| | `=~`、`$1`-`$9` キャプチャグループ |
| | `match?`, `gsub`, `sub`, `scan` (ブロック付き), `split` |
| | Regexp不使用プログラムではoniguruma不要 |
| **演算** | 算術 (+, -, *, /, %, **), 比較, ビット演算 |
| | 単項マイナス, 複合代入 (+=, <<=) |
| | Math.sqrt/cos/sin → C math関数 |
| | Integer#abs/even?/odd?/zero?/positive?/negative? |
| | Float#abs/ceil/floor/round |
| **文字列** | リテラル、補間 → printf |
| | 15+メソッド: length, upcase, downcase, strip, reverse |
| |   gsub, sub, split, capitalize, chomp |
| |   include?, start_with?, end_with?, count |
| |   +, <<, * (連結、追記、繰り返し) |
| |   ==, !=, <, > (strcmp比較) |
| | Integer#to_s, Integer#chr |
| **I/O** | puts, print, printf, putc, p → stdio |
| | puts: Integer, Float, Boolean, String対応 |
| **GC** | マーク&スイープ (非値型オブジェクト・配列・ハッシュ用) |
| | シャドウスタックルート管理, ファイナライザ |
| | GC不要なプログラムではGCコード省略 |

### テストプログラム (23例)

| プログラム | テスト対象 |
|-----------|-----------|
| bm_so_mandelbrot | while、ビット演算、PBM出力 |
| bm_ao_render | 6クラス、モジュール、GC |
| bm_so_lists | 配列操作 (push/pop/shift)、GC |
| bm_fib | 再帰、関数型推論 |
| bm_app_lc_fizzbuzz | 1201クロージャ、アリーナ |
| bm_mandel_term | 関数間呼び出し、putc |
| bm_yield | yield/ブロック、each/map/select |
| bm_case | case/when、unless、next、デフォルト引数 |
| bm_inherit | 継承、super |
| bm_rescue | rescue/raise/ensure/retry |
| bm_hash | Hash操作 |
| bm_strings | Symbol、基本文字列メソッド |
| bm_strings2 | 高度な文字列メソッド、split、比較 |
| bm_numeric | 数値メソッド (abs, ceil, even?, **) |
| bm_attr | attr_accessor、for..in、loop、クラスメソッド |
| bm_kwargs | キーワード引数、スプラット |
| bm_mixin | include (mixin) |
| bm_misc | upto/downto、String <<、配列引数 |
| bm_regexp | 正規表現 (=~, $1, match?, gsub, sub, scan, split) |
| bm_introspect | is_a?, respond_to?, nil?, positive?, negative? |
| bm_struct | Struct.new |
| bm_array2 | Array#reject/first/last/include? |
| bm_sort_reduce | Array#sort/min/max/sum/reduce/inject |

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
Regexp使用時のみ libonig をリンク。

---

## 未サポート機能

### 高優先度

| 機能 | 備考 |
|------|------|
| 多値Hash (任意型value) | 現在はString→Integerのみ |
| `Comparable`, `Enumerable` | モジュール組み込み |
| `extend` | クラスレベルmixin |
| `Proc.new`, `proc {}` | lambda以外のProc |
| `alias` | メソッド別名 |
| `Array#reduce/inject` | 畳み込み |
| `Array#sort/sort_by` | ソート |

### 中優先度

| 機能 | 備考 |
|------|------|
| 多段継承チェーン | 現在は1段のみテスト済み |
| Exception クラス定義 | 現在は文字列のみ |
| `Data` クラス | Ruby 3.2+ |
| `**kwargs` (ダブルスプラット) | ハッシュ引数 |
| `Hash` with non-string keys | 任意キー型 |
| `String#[]`/`String#[]=` | 文字列インデックス |
| `class` メソッド | クラス名取得 |
| `freeze` / `frozen?` | イミュータブル |

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
Pass 1: クラス解析              -- クラス (継承チェーン)、メソッド、ivar検出
    |                              モジュール (mixin解決)、attr_accessor展開
    |                              Struct.new展開、トップレベル関数、yield検出
    v
Pass 2: 型推論                  -- 全変数・ivar・パラメータの型推論
    |                              (Integer/Float/Boolean/String/Object/Array/Hash/Proc/Regexp)
    |                              関数間型推論、super型伝播、継承ivar伝播
    |                              キーワード引数・スプラットの型解決
    v
Pass 3: 構造体・メソッド生成    -- クラス → C構造体 (親フィールド先頭配置)
    |                              メソッド → C関数 (継承はcast-to-parent)
    |                              getter/setter → インラインフィールドアクセス
    |                              is_a?/respond_to? → コンパイル時定数
    |                              GCスキャン関数、ファイナライザ生成
    |                              ラムダ → キャプチャ解析 + C関数生成
    |                              yield → コールバック関数ポインタ生成
    |                              Regexp → onigurumaプリコンパイル
    v
Pass 4: main() コード生成       -- トップレベルコード → main()
    |                              while/for/times/each/upto/downto → Cループ
    |                              yield → _block(_block_env, arg)
    |                              case/when → if/else チェーン
    |                              rescue → setjmp/longjmp
    |                              =~ → onig_search
    |                              算術 → C演算子
    |                              puts/print/printf → stdio
    v
スタンドアロンCファイル           -- GC内蔵, 例外処理内蔵
    |
    v
cc -O2 -lm [-lonig] → ネイティブバイナリ
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

# Regexp使用プログラムのコンパイル
./spinel --source=examples/bm_regexp.rb --output=regexp.c
cc -O2 regexp.c -lonig -lm -o regexp
./regexp

# テスト
make test   # mandelbrotをコンパイル・実行・CRubyと出力比較
```

## プロジェクト構成

```
spinel/
├── src/
│   ├── main.c          # CLI、ファイル読み込み、Prismパース
│   ├── codegen.h       # 型システム、クラス/メソッド/モジュール情報構造体
│   └── codegen.c       # 多パスコード生成器 (~7400行)
├── examples/           # 23テストプログラム
│   ├── bm_so_mandelbrot.rb   # Mandelbrot集合
│   ├── bm_ao_render.rb       # AOレイトレーサー (6クラス、モジュール)
│   ├── bm_so_lists.rb        # 配列操作
│   ├── bm_fib.rb             # 再帰フィボナッチ
│   ├── bm_app_lc_fizzbuzz.rb # λ計算FizzBuzz (1201クロージャ)
│   ├── bm_mandel_term.rb     # ターミナルMandelbrot
│   ├── bm_yield.rb           # yield/ブロック
│   ├── bm_case.rb            # case/when, unless, next
│   ├── bm_inherit.rb         # 継承、super
│   ├── bm_rescue.rb          # rescue/raise/ensure/retry
│   ├── bm_hash.rb            # Hash操作
│   ├── bm_strings.rb         # Symbol、文字列メソッド
│   ├── bm_strings2.rb        # 高度な文字列メソッド
│   ├── bm_numeric.rb         # 数値メソッド
│   ├── bm_attr.rb            # attr_accessor、for..in、loop、クラスメソッド
│   ├── bm_kwargs.rb          # キーワード引数、スプラット
│   ├── bm_mixin.rb           # include (mixin)
│   ├── bm_misc.rb            # upto/downto、String <<
│   ├── bm_regexp.rb          # 正規表現 (oniguruma)
│   ├── bm_introspect.rb      # is_a?, respond_to?, nil?
│   ├── bm_struct.rb          # Struct.new
│   └── bm_array2.rb          # Array#reject/first/last/include?
├── prototype/
│   └── tools/          # Step 0プロトタイプ (RBS抽出、LumiTrace等)
├── Makefile
├── PLAN.md             # 本文書
└── ruby_aot_compiler_design.md  # 詳細設計文書
```

## 完了した次ステップ (7項目の評価)

| # | 項目 | 結果 |
|---|------|------|
| 1 | **多値Hash** | **保留** — ボックス化が必要で大規模な変更。String→Integerで多くのケースに対応。 |
| 2 | **Array#sort / sort_by** | **完了** ✅ — qsortベースのsort/sort!、min/max/sum追加 |
| 3 | **Array#reduce / inject** | **完了** ✅ — インライン畳み込み、ブロックパラメータの型推論対応 |
| 4 | **Proc.new / proc {}** | **保留** — lambda構文(`-> x { }`)で大半のケースに対応。proc意味論の差異(returnの挙動)は要検討。 |
| 5 | **`extend`** | **保留** — `include`より使用頻度低。必要時に追加。 |
| 6 | **LumiTraceプロファイル統合** | **保留** — 静的型推論が23例で十分機能。動的プロファイルは型が静的に決定できないプログラムで有用。 |
| 7 | **複数ファイルコンパイル** | **保留** — require/loadのファイル解決、定義のマージが必要な大規模変更。単一ファイルで全例に対応。 |

## 「全Rubyプログラムのコンパイル」に向けて

### 現状の限界

現在のSpinelは**静的単相型**(各変数に1つの型)を前提とする。実際のRubyは動的型で、
任意の変数が任意の型の値を持てる。この差が「コンパイルできないプログラム」の主因。

```ruby
# 現在コンパイルできないパターン:
x = 1; x = "hello"            # 変数の型が変わる
arr = [1, "two", 3.0]         # 異種配列
def parse(s)                   # 戻り値の型が条件で変わる
  s =~ /^\d+$/ ? s.to_i : s
end
def make_noise(obj)            # ダックタイピング
  obj.speak                    # objの型が不定
end
```

### 未対応機能の分類

| レイヤー | 未対応項目 | ポリモーフィズムとの関連 |
|---------|-----------|---------------------|
| **型システム** | 多相変数、異種配列/Hash、Union型 | **核心問題** |
| **ディスパッチ** | ダックタイピング、send、method_missing | 型に依存 |
| **Proc/Block** | Proc.new、&block、block_given? | 部分的に型問題 |
| **組込クラス** | IO, File, Time, Encoding, Range-as-object | 型とは独立 |
| **メタ** | eval, define_method, open class | 静的解析の限界 |
| **構成** | require/load, gem | アーキテクチャ問題 |

---

## ポリモーフィズム設計

### 方針: ハイブリッド型システム (Crystal方式)

現在の**単相最適化を維持**しつつ、必要な箇所にのみ**ボックス化**を導入する。

```
型推論の結果:
  変数が常に1つの型 → 現在通り: mrb_int, mrb_float, sp_Vec, etc. (アンボックス)
  変数が複数の型    → sp_RbValue (ボックス化タグ付きユニオン)
```

### sp_RbValue: 汎用ボックス型

```c
// Phase 1: 16バイトタグ付きユニオン (シンプル、デバッグ容易)
enum sp_tag {
    SP_T_INT, SP_T_FLOAT, SP_T_BOOL, SP_T_NIL,
    SP_T_STRING, SP_T_SYMBOL, SP_T_ARRAY, SP_T_HASH,
    SP_T_OBJECT, SP_T_PROC, SP_T_REGEXP
};

typedef struct {
    enum sp_tag tag;
    union {
        int64_t i;       // SP_T_INT
        double f;        // SP_T_FLOAT
        const char *s;   // SP_T_STRING, SP_T_SYMBOL
        void *p;         // SP_T_OBJECT, SP_T_ARRAY, SP_T_HASH, SP_T_PROC
    };
} sp_RbValue;  // 16 bytes

// Phase 2 (将来): NaN-boxing (8バイト、高速)
// IEEE 754 NaN空間にInteger/ポインタをエンコード
```

### 型推論の拡張: Union型

```
現在: x: Integer | x: Float | x: VALUE (フォールバック)
拡張: x: Integer | Float   → sp_RbValue (Union型)
      x: Duck | Person     → sp_RbValue (ダックタイピング)
      x: Integer            → mrb_int (アンボックス維持)
```

型推論で変数に**複数の型が代入される**ことを検出したら、`SPINEL_TYPE_POLY`に昇格:
```c
// var_declare()でのwidening:
// 現在: Integer + Float → Float (暗黙変換)
// 拡張: Integer + String → POLY (ボックス化が必要)
```

### メソッドディスパッチ

**単相 (現在通り)**: 型が確定 → 直接C関数呼び出し
```c
sp_Duck_speak(lv_obj);  // objがDuckと確定
```

**多相 (新規)**: Union型 → コンパイル時に既知の型リストでswitch
```c
// objがDuck | Personの場合 (Crystal方式のinline dispatch)
switch (lv_obj.tag) {
    case SP_T_DUCK:   sp_Duck_speak((sp_Duck *)lv_obj.p); break;
    case SP_T_PERSON: sp_Person_speak((sp_Person *)lv_obj.p); break;
    default:          sp_raise("NoMethodError: speak"); break;
}
```

**メガモーフィック** (3型以上 or 型不定): vtable dispatch
```c
// objの型がコンパイル時に確定できない場合
sp_vtable_call(lv_obj, "speak", 0);  // ハッシュテーブルベース
```

### ボックス化/アンボックス化の境界

```ruby
def add(a, b)   # a: Integer (確定), b: Integer (確定)
  a + b         # → アンボックス: lv_a + lv_b
end

def show(x)     # x: Integer | String (多相)
  puts x        # → sp_RbValue_puts(lv_x)
end

n = add(1, 2)   # n: Integer → アンボックス
show(n)          # Integer → sp_RbValue への boxing が発生
show("hello")   # String → sp_RbValue への boxing
```

Boxing/Unboxingコード生成:
```c
// Boxing: mrb_int → sp_RbValue
sp_RbValue sp_box_int(mrb_int n) { return (sp_RbValue){SP_T_INT, .i = n}; }

// Unboxing: sp_RbValue → mrb_int (型チェック付き)
mrb_int sp_unbox_int(sp_RbValue v) {
    if (v.tag != SP_T_INT) sp_raise("TypeError");
    return v.i;
}
```

### 実装ロードマップ

#### Phase 1: sp_RbValue基盤 + 基本操作
- `sp_RbValue` 16バイトタグ付きユニオンの定義
- Boxing/Unboxing関数群
- sp_RbValue上の基本演算 (+, -, *, /, ==, <, puts等)
- `SPINEL_TYPE_POLY` の追加
- 型推論のwidening: Integer + String → POLY

#### Phase 2: 多相メソッドディスパッチ
- Union型の追跡 (変数が持ちうる型の集合)
- 2-3型のUnion → switch-based inline dispatch
- 多相メソッドのコード生成

#### Phase 3: 異種コレクション
- `Array<sp_RbValue>` — 任意型の要素を持つ配列
- `Hash<sp_RbValue, sp_RbValue>` — 任意型のキー/値

#### Phase 4: ダックタイピング
- メソッド名ベースのディスパッチ (vtable or hash)
- `respond_to?` の動的版
- `send` / `public_send`

#### Phase 5: 最適化
- NaN-boxing (8バイト化)
- Escape analysis (ボックス化の回避)
- Inline cache (ディスパッチの高速化)
- LumiTraceプロファイル統合 (speculative optimization)

### 設計原則

1. **段階的導入**: 既存の単相コンパイルを壊さない。POLYは必要な変数にのみ適用。
2. **性能優先**: 単相パスは現在の速度を維持。多相はCrystal程度の速度を目標。
3. **互換性**: 最終的に全てのvalid Rubyプログラムをコンパイル可能に。
4. **NaN-boxing準備**: Phase 1のsp_RbValueをPhase 5でNaN-boxingに置き換え可能な設計。

---

## 全Rubyコンパイルへの残課題 (10カテゴリ)

### 1. 動的型付け / ポリモーフィズム 【核心問題 — 上記設計で対応】
- 複数型を持つ変数 (`x = 1; x = "hello"`)
- 異種配列 (`[1, "two", :three, 4.0]`)
- 異種Hash (`{a: 1, b: "two"}`)
- 条件で戻り値型が変わるメソッド
- → **sp_RbValue + Union型 + switch dispatch** で対応

### 2. require / load / gem
- 複数ファイルプログラム、標準ライブラリ、gem
- `$LOAD_PATH`, `require_relative`
- → ファイル解決 + AST統合。単一バイナリにリンク or 動的ロード。
- 実装方針: Prismで各ファイルをパース → クラス/関数登録をマージ → 統合コード生成

### 3. 完全なBlock/Proc意味論
- `Proc.new`, `proc {}`, `method(:name)`, `&block` パラメータ
- `block_given?`
- Proc vs lambda の return 意味論の差異
- スコープを超えるProc (クロージャのライフタイム管理)
- → lambda構文は対応済み。Proc.newはsp_RbValue + クロージャランタイムの統合で対応。

### 4. 未対応組込クラス
- `IO`, `File`, `Dir` — ファイルI/O (Cの stdio/fopen でラップ)
- `Encoding` — 文字列エンコーディング (UTF-8前提で簡略化可)
- `Thread`, `Fiber` (基本的なもの以外)
- `Time`, `Date` — 時刻 (time.h / strftime)
- `Set` — ハッシュセット
- `Range` as object — for..in以外のRange使用 (each, include?, etc.)
- `Complex`, `Rational` — 数値型
- `Enumerator`, `Enumerator::Lazy`
- → 各クラスをC構造体 + メソッドセットとして実装。GC統合。

### 5. 完全なString
- ミュータブル文字列 (現在は `const char *`)
- エンコーディング対応操作
- `String#[]`, `#[]=`, `#freeze`, `#frozen?`
- `#match` returning MatchData
- `#encode`, `#bytes`, `#chars`, `#each_char`
- → sp_String構造体 (data + length + capacity + encoding) で置き換え

### 6. オブジェクトシステムの完全性
- `method_missing`, `respond_to_missing?`
- `send`, `public_send`
- `define_method`, `class_eval`, `instance_eval`
- open class (クラス再オープン、monkey patching)
- Singleton class (`class << obj`)
- `protected`, `private`, `public` アクセス制御
- `alias_method`
- `Module#ancestors`
- `BasicObject`
- → メタプログラミング系は**インタプリタフォールバック**で対応。
  mrubyをリンクし、静的解析不能な部分をmrubyに委譲。

### 7. 制御フローの完全性
- `catch` / `throw`
- `BEGIN` / `END`
- `defined?`
- `__method__`, `__FILE__`, `__LINE__`
- → 多くはコンパイル時定数で解決可能。catch/throwはsetjmp/longjmpの拡張。

### 8. パターンマッチ (Ruby 3.0+)
- `case/in`
- Pin operator (`^variable`)
- Find pattern, array/hash pattern
- → case/whenの拡張。型チェック + 分配束縛のコード生成。

### 9. 完全な例外階層
- Exception クラス定義 (ユーザー定義例外)
- `rescue TypeError`, `rescue ArgumentError => e`
- カスタム例外クラスの継承
- `ensure` in method bodies (begin外)
- → 現在の文字列例外を、クラスベース例外に拡張。例外オブジェクトをsp_RbValueで保持。

### 10. GCの完全性
- 現在: 構造体のみGC対象。文字列はmallocで漏れる。
- ラムダクロージャはアリーナ（個別解放不可）。
- 長時間動作プログラムで文字列リーク。
- → Phase 1: 文字列をGC対象に (sp_String + ファイナライザ)
- → Phase 2: 世代別GC (nursery/old)
- → Phase 3: コンパクションGC (メモリフラグメント対策)

## 参考情報

- 詳細設計: `ruby_aot_compiler_design.md`
- プロトタイプツール: `prototype/`
- 参考実装:
  - **Crystal**: Union型 + コンパイル時switch dispatch
  - **TruffleRuby**: Speculative optimization + deoptimization
  - **Sorbet**: 漸進的型付け (T.untyped fallback)
  - **mruby**: NaN-boxing mrb_value + vtable dispatch
