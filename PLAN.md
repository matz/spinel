# PLAN: Spinel AOT Compiler

Ruby source → Prism AST → whole-program type inference → standalone C executable.
No runtime dependencies (no mruby, no GC library — GC is generated inline).
Regexp対応プログラムのみ libonig をリンク。

詳細設計は `ruby_aot_compiler_design.md` を参照。

---

## 現状 (Status)

### コンパイラアーキテクチャ (~9700行のC)

- Prism (libprism) によるRubyパース
- 多パスコード生成:
  1. クラス/モジュール/関数解析 (継承チェーン、mixin解決、Struct.new展開含む)
  2. 全変数・パラメータ・戻り値の型推論 (関数間解析)
  3. C構造体・メソッド関数の生成 (GCスキャン関数含む)
  4. ラムダ/クロージャのキャプチャ解析・コード生成
  5. yield/ブロックのコールバック関数生成 (block_given?対応)
  6. 正規表現パターンのプリコンパイル (oniguruma)
  7. main()のトップレベルコード生成
- マーク&スイープGC (シャドウスタック、ファイナライザ)
- setjmp/longjmpベース例外処理 (クラス例外階層対応)
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
| | `alias` — メソッド別名 |
| | `freeze`/`frozen?` — AOTでは全値がfrozen扱い |
| | getter/setter自動インライン化 |
| | コンストラクタ (`.new`)、型付きオブジェクトへのメソッド呼び出し |
| | モジュール (状態変数 + メソッド) |
| **イントロスペクション** | `is_a?` — 継承チェーンをコンパイル時に静的解決 |
| | `respond_to?` — メソッドテーブルをコンパイル時に静的解決 |
| | `nil?` — nil以外は常にFALSE |
| | `defined?` — 変数定義チェック (コンパイル時) |
| **ブロック/クロージャ** | `yield`、ブロック付きメソッド呼び出し (キャプチャ変数) |
| | `block_given?` — ブロックの有無チェック |
| | `Array#each/map/select/reject/reduce/inject` (インライン化) |
| | `Hash#each` (キー/値ペア) |
| | `Integer#times/upto/downto` with block → C forループ |
| | `-> x { body }` ラムダ → Cクロージャ (キャプチャ解析) |
| **制御** | while, until, if/elsif/else, unless |
| | case/when/else (値、複数値、Range条件) |
| | for..in + Range, loop do |
| | break, next, return |
| | ternary, and/or/not |
| | `__LINE__`, `__FILE__`, `__method__`, `defined?` |
| | `catch`/`throw` (タグ付き非局所脱出) |
| **例外処理** | begin/rescue/ensure/retry |
| | `raise "message"`, `raise ClassName, "message"` |
| | `rescue ClassName => e` (クラス階層チェック付き) |
| | 複数rescue節の連鎖 |
| | volatile変数でlongjmpの値保存 |
| **引数** | 位置引数、デフォルト値 (`def foo(x = 10)`) |
| | キーワード引数 (`def foo(name:, greeting: "Hello")`) |
| | 可変長引数/スプラット (`def sum(*nums)`) |
| **型** | Integer, Float, Boolean, String, Symbol, nil → アンボックスC型 |
| | 値型 (Vec: 3 floats → 値渡し) vs ポインタ型 |
| **コレクション** | sp_IntArray (push/pop/shift/dup/reverse!/each/map/select/reject/reduce) |
| | Array#first/last/include?/sort/sort!/min/max/sum/length |
| | sp_StrIntHash (文字列キー→整数値、each/has_key?/delete) |
| | sp_StrArray (文字列配列、split結果用) |
| | O(1) shift (デキュー方式のstartオフセット) |
| **正規表現** | `/pattern/` リテラル → onigurumaプリコンパイル |
| | `=~`、`$1`-`$9` キャプチャグループ |
| | `match?`, `gsub`, `sub`, `scan` (ブロック付き), `split` |
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
| | puts: Integer, Float, Boolean, String対応 (末尾改行のRuby互換) |
| | File.read, File.write, File.exist?, File.delete |
| **GC** | マーク&スイープ (非値型オブジェクト・配列・ハッシュ用) |
| | シャドウスタックルート管理, ファイナライザ |
| | GC不要なプログラムではGCコード省略 |

### テストプログラム (40例)

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
| bm_control | __LINE__, __FILE__, defined? |
| bm_exceptions | raise ClassName, rescue ClassName, 例外階層 |
| bm_block2 | block_given?, ブロック付きyield呼び出し |
| bm_fileio | File.read/write/exist?/delete |
| bm_catch | catch/throw (タグ付き非局所脱出) |
| bm_features | __method__, freeze/frozen? |
| bm_comparable | Comparable演算子メソッド、alias |
| bm_range | Range as object (first, last, each, include?, to_a) |
| bm_time | Time.now, Time.at, to_i, 差分 |
| bm_enumerable | Enumerable, yield付きeachメソッド |
| bm_method | method(:name) → sp_Proc |
| bm_strindex | String#[] (文字インデックス) |
| bm_stdlib | ARGV, $stderr, srand/rand, exit |
| bm_proc | &block, proc {}, Proc.new, Proc#call |
| bm_poly | 多相変数 (sp_RbValue Phase 1) |
| bm_poly2 | 異種配列, bimorphicダックタイピング (Phase 2) |
| bm_pattern | パターンマッチ case/in (Phase 3) |

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

## 全Rubyコンパイルへの残課題 (10カテゴリ)

| # | カテゴリ | 状態 | 次のアクション |
|---|---------|------|-------------|
| 1 | **動的型付け / ポリモーフィズム** | **Phase 2完了** ✅ | Phase 3: パターンマッチ |
| 2 | **require / load / gem** | 未着手 | ファイル解決 + AST統合 |
| 3 | **Block/Proc完全性** | **完了** ✅ | yield, &block, Proc.new, proc {}, method(:name) |
| 4 | **組込クラス** | **ほぼ完了** ✅ | File, Time, Range, Enumerable 対応済み |
| 5 | **完全なString** | 未着手 | sp_String構造体 (ミュータブル + encoding) |
| 6 | **オブジェクトシステム完全性** | 一部完了 | Comparable完了。method_missing等はフォールバック |
| 7 | **制御フロー完全性** | **完了** ✅ | catch/throw含む全制御フロー |
| 8 | **パターンマッチ** | **完了** ✅ | case/in (型/値/nil/alternation) |
| 9 | **例外階層** | **完了** ✅ | raise ClassName, rescue ClassName, 継承チェック |
| 10 | **GC完全性** | 一部完了 | 文字列GC (sp_String), 世代別GC |

### 完了した項目
- ✅ **sp_RbValue Phase 1**: 多相変数、boxing/unboxing、sp_poly_puts、nil?
- ✅ **sp_RbValue Phase 2**: 異種配列(sp_RbArray)、bimorphicダックタイピング、クラスタグ
- ✅ **パターンマッチ**: `case/in` (型チェック、値マッチ、AlternationPattern、nil)
- ✅ **Block/Proc**: yield, block_given?, &block, proc {}, Proc.new, Proc#call, method(:name)
- ✅ **Comparable**: 演算子メソッドC名サニタイズ (<=> → _cmp等)、self参照
- ✅ **Range as object**: first, last, each, include?, to_a, sum
- ✅ **Time**: Time.now, Time.at, to_i, 差分
- ✅ **Enumerable**: yield付きeachメソッドのブロック対応
- ✅ `catch`/`throw`, `alias`, `freeze`/`frozen?`, `__method__`, `sleep`
- ✅ `ARGV`, `$stderr.puts`, `exit`, `srand`/`rand`, `String#[]`
- ✅ `File.read/write/exist?/delete`
- ✅ `Array#sort/sort!/min/max/sum/reduce/inject/join/uniq`

---

## ポリモーフィズム設計

### 方針: ハイブリッド型 + 3段階ディスパッチ

現在の**単相最適化を維持**しつつ、必要な箇所にのみ**ボックス化**を導入する。
ディスパッチは多相度に応じた3段階方式。

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
```

### 3段階メソッドディスパッチ

JITコンパイラのインラインキャッシュと同じ発想。多相度に応じて最適な方式を選択。

| 多相度 | 名称 | 方式 | コード例 | 速度 |
|--------|------|------|---------|------|
| 1型 | **monomorphic** | 直接呼び出し (現行) | `sp_Duck_speak(obj)` | 最速 |
| 2型 | **bimorphic** | call-site inline switch | `if (obj.tag == T_DUCK) ... else ...` | 高速 |
| 3型以上 | **megamorphic** | dispatch関数 | `sp_dispatch_speak(obj)` | 中速 |

#### Monomorphic (型が1つに確定 — 変更なし)

```c
// 型推論で obj: Duck と確定 → 直接呼び出し
sp_Duck_speak(lv_obj);
```

現在の全37例がこのパス。**性能影響ゼロ**。

#### Bimorphic (2型のUnion — call-site switch)

```c
// 型推論で obj: Duck | Person と判明 → call siteでswitch
if (lv_obj.tag == SP_T_DUCK)
    result = sp_box_str(sp_Duck_speak((sp_Duck *)lv_obj.p));
else
    result = sp_box_str(sp_Person_speak((sp_Person *)lv_obj.p));
```

実用上最も頻出するケース:
- `Integer | Float` (数値演算)
- `SomeClass | nil` (nilable)
- `ClassA | ClassB` (2種の具象型)

関数呼び出しオーバーヘッドゼロ。Cコンパイラの分岐予測最適化が効く。

#### Megamorphic (3型以上 — dispatch関数)

```c
// コンパイル末尾で自動生成: speakを持つ全クラスを集約
sp_RbValue sp_dispatch_speak(sp_RbValue obj) {
    switch (obj.tag) {
        case SP_T_DUCK:   return sp_box_str(sp_Duck_speak((sp_Duck *)obj.p));
        case SP_T_PERSON: return sp_box_str(sp_Person_speak((sp_Person *)obj.p));
        case SP_T_ROBOT:  return sp_box_str(sp_Robot_speak((sp_Robot *)obj.p));
        default:          sp_raise("NoMethodError: speak");
    }
}

// call siteはシンプル
result = sp_dispatch_speak(lv_obj);
```

dispatch関数はメソッド名ごとに1つ。コンパイル時に全クラス情報があるため、
実行時ハッシュ不要の有限switch文で実装。

### Boxing / Unboxing

```c
// Boxing: アンボックス型 → sp_RbValue
sp_RbValue sp_box_int(int64_t n)      { return (sp_RbValue){SP_T_INT,    .i = n}; }
sp_RbValue sp_box_float(double f)     { return (sp_RbValue){SP_T_FLOAT,  .f = f}; }
sp_RbValue sp_box_str(const char *s)  { return (sp_RbValue){SP_T_STRING, .s = s}; }
sp_RbValue sp_box_bool(int b)         { return (sp_RbValue){SP_T_BOOL,   .i = b}; }
sp_RbValue sp_box_nil(void)           { return (sp_RbValue){SP_T_NIL,    .i = 0}; }

// Unboxing: sp_RbValue → アンボックス型 (型チェック付き)
int64_t sp_unbox_int(sp_RbValue v)    { return v.i; }
double sp_unbox_float(sp_RbValue v)   { return v.tag == SP_T_FLOAT ? v.f : (double)v.i; }
const char *sp_unbox_str(sp_RbValue v){ return v.s; }
```

Boxing/Unboxingは**MONO↔POLY境界**でのみ発生:
```ruby
def add(a, b)   # a: Integer (MONO) → アンボックスのまま
  a + b         # → lv_a + lv_b (直接C演算)
end

def show(x)     # x: Integer | String (POLY) → sp_RbValue
  puts x        # → sp_dispatch_puts(lv_x)
end

n = add(1, 2)   # n: Integer (MONO)
show(n)          # ← ここでboxingが発生: sp_box_int(lv_n)
show("hello")   # ← sp_box_str("hello")
```

### 組込型の演算ディスパッチ

```c
// sp_RbValue上の + 演算 (bimorphic: Integer | Float)
if (a.tag == SP_T_INT && b.tag == SP_T_INT)
    result = sp_box_int(a.i + b.i);           // 整数加算
else
    result = sp_box_float(                     // Float昇格
        (a.tag == SP_T_FLOAT ? a.f : (double)a.i) +
        (b.tag == SP_T_FLOAT ? b.f : (double)b.i));

// sp_RbValue上の puts (megamorphic: 全型)
sp_RbValue sp_dispatch_puts(sp_RbValue v) {
    switch (v.tag) {
        case SP_T_INT:    printf("%lld\n", (long long)v.i); break;
        case SP_T_FLOAT:  { char buf[32]; snprintf(buf,32,"%g",v.f);
                            printf("%s%s\n", buf, strchr(buf,'.')||strchr(buf,'e') ? "" : ".0"); break; }
        case SP_T_STRING: { fputs(v.s, stdout); if (!*v.s || v.s[strlen(v.s)-1]!='\n') putchar('\n'); break; }
        case SP_T_BOOL:   puts(v.i ? "true" : "false"); break;
        case SP_T_NIL:    puts(""); break;
        default:          puts("(object)"); break;
    }
    return sp_box_nil();
}
```

### 型推論の拡張

```
現在: 変数に1つの型を割り当て (MONO)
      Integer + Float → Float (暗黙変換)
      Integer + String → VALUE (フォールバック — 実質エラー)

拡張: Union型を追跡 (POLY)
      Integer + Float → Float (変換可能なら維持)
      Integer + String → POLY{Integer, String} (boxing)
      Duck + Person → POLY{Duck, Person} (bimorphic dispatch)
      Duck + Person + Robot → POLY{Duck, Person, Robot} (megamorphic dispatch)
```

### 実装ロードマップ

| Phase | 内容 | 状態 |
|-------|------|------|
| **1a** | sp_RbValue型定義 + boxing/unboxing関数 | ✅ 完了 |
| **1b** | sp_RbValue上の基本演算 (puts, nil?等) | ✅ 完了 |
| **1c** | SPINEL_TYPE_POLY + Union型追跡 | ✅ 完了 |
| **1d** | bimorphic call-site switch生成 | ✅ 完了 |
| **2a** | 異種配列 `[1, "two", 3.0]` → sp_RbArray | ✅ 完了 |
| **2b** | bimorphicダックタイピング (クラスタグ) | ✅ 完了 |
| **2c** | 異種Hash `{a: 1, b: "str"}` | 未着手 |
| **3** | パターンマッチ `case/in` (型チェック分岐) | ✅ 完了 |
| **4** | megamorphic dispatch関数生成 (3型以上) | 未着手 |
| **5** | sp_String (ミュータブル文字列 + GC) | 未着手 |
| **6** | NaN-boxing (8バイト化) | 未着手 |

### 設計原則

1. **段階的導入**: 既存の単相コンパイルを壊さない。POLYは必要な変数にのみ適用
2. **3段階最適化**: mono→直接, bi→inline switch, mega→dispatch関数
3. **性能優先**: 単相パスは現在の速度 (20-57× vs CRuby) を維持
4. **互換性**: 最終的に全valid Rubyをコンパイル可能に
5. **NaN-boxing準備**: Phase 1のsp_RbValueをPhase 5で8バイト化可能な設計

### sp_RbValue完了により解放された機能
- ✅ 多相変数 (x = 1; x = "hello")
- ✅ 異種配列 [1, "two", 3.0]
- ✅ bimorphicダックタイピング (2クラス)
- ✅ nilable変数

### 次の3ステップ

| # | 内容 | 設計 |
|---|------|------|
| 1 | **異種Hash** `{a: 1, b: "str"}` | sp_RbHashでキー/値をsp_RbValueで保持 |
| 2 | **POLY算術** (+, -, *, / on sp_RbValue) | タグcheckでInt/Float分岐、String連結対応 |
| 3 | **Megamorphic dispatch** (3型以上) | メソッド名ごとにdispatch関数を自動生成 |

---

## プロジェクト構成

```
spinel/
├── src/
│   ├── main.c          # CLI、ファイル読み込み、Prismパース
│   ├── codegen.h       # 型システム、クラス/メソッド/モジュール情報構造体
│   └── codegen.c       # 多パスコード生成器 (~9700行)
├── examples/           # 40テストプログラム
├── prototype/
│   └── tools/          # Step 0プロトタイプ (RBS抽出、LumiTrace等)
├── Makefile
├── PLAN.md             # 本文書
└── ruby_aot_compiler_design.md  # 詳細設計文書
```

## ビルドフロー

```bash
make deps && make         # コンパイラビルド
./spinel --source=app.rb --output=app.c
cc -O2 app.c -lm -o app  # Regexp使用時は -lonig 追加
```

## 参考情報

- 詳細設計: `ruby_aot_compiler_design.md`
- プロトタイプツール: `prototype/`
- 参考実装: Crystal, TruffleRuby, Sorbet, mruby
