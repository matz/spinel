# A long if / elsif / else ladder (and a nested ternary) emits C as long as
# the Ruby is wide, not as deep: every arm continues the one `if` chain
# (`else if`) instead of opening another block, and a value ladder assigns
# each arm into its one result temp. Linux clang stops a translation unit at
# 256 nested brackets, so 260 arms in each of the four ladders below failed
# to build there (Apple clang allows more, so a macOS run does not show it).
# The four ladders are a statement ladder, a value ladder with statements in
# its arms, a plain nested ternary and a nested ternary whose conditions hoist
# setup (an Array argument); the statement and value ladders also hold
# conditions that hoist setup (it runs only when the arm is reached) and
# `else` arms holding a hand-written `if` or `unless`. The short shapes after
# them cover the other positions and arm kinds in a few arms each: a method's
# tail, a block's and a lambda's value, arms the compiler decides, arms of
# five classes, a ladder with no else, a mutable String arm, `unless`, a
# diverging arm, return / next in an elsif or hand-written `else` + `if` arm
# and break in an elsif arm of a value ladder, and the order of side effects.
xs = [0, 1, 150, 258, 259, 260, -5]
$seen = []
def chk(a) = ($seen << a[1]; a[0] == a[1])
$log = []
def g(v) = ($log << v; v)


def lad_a(x)
  if x == 0 then puts "a0"
  elsif x == 1 then puts "a1"
  elsif x == 2 then puts "a2"
  elsif x == 3 then puts "a3"
  elsif x == 4 then puts "a4"
  elsif x == 5 then puts "a5"
  elsif x == 6 then puts "a6"
  elsif chk([x, 7]) then puts "a7"
  elsif x == 8 then puts "a8"
  elsif x == 9 then puts "a9"
  elsif x == 10 then puts "a10"
  elsif x == 11 then puts "a11"
  elsif x == 12 then puts "a12"
  elsif x == 13 then puts "a13"
  elsif x == 14 then puts "a14"
  elsif x == 15 then puts "a15"
  elsif x == 16 then puts "a16"
  elsif chk([x, 17]) then puts "a17"
  elsif x == 18 then puts "a18"
  elsif x == 19 then puts "a19"
  elsif x == 20 then puts "a20"
  elsif x == 21 then puts "a21"
  elsif x == 22 then puts "a22"
  elsif x == 23 then puts "a23"
  elsif x == 24 then puts "a24"
  elsif x == 25 then puts "a25"
  elsif x == 26 then puts "a26"
  elsif chk([x, 27]) then puts "a27"
  elsif x == 28 then puts "a28"
  elsif x == 29 then puts "a29"
  elsif x == 30 then puts "a30"
  elsif x == 31 then puts "a31"
  elsif x == 32 then puts "a32"
  elsif x == 33 then puts "a33"
  elsif x == 34 then puts "a34"
  elsif x == 35 then puts "a35"
  elsif x == 36 then puts "a36"
  elsif chk([x, 37]) then puts "a37"
  elsif x == 38 then puts "a38"
  elsif x == 39 then puts "a39"
  elsif x == 40 then puts "a40"
  elsif x == 41 then puts "a41"
  elsif x == 42 then puts "a42"
  elsif x == 43 then puts "a43"
  elsif x == 44 then puts "a44"
  elsif x == 45 then puts "a45"
  elsif x == 46 then puts "a46"
  elsif chk([x, 47]) then puts "a47"
  elsif x == 48 then puts "a48"
  elsif x == 49 then puts "a49"
  elsif x == 50 then puts "a50"
  elsif x == 51 then puts "a51"
  elsif x == 52 then puts "a52"
  elsif x == 53 then puts "a53"
  elsif x == 54 then puts "a54"
  elsif x == 55 then puts "a55"
  elsif x == 56 then puts "a56"
  elsif chk([x, 57]) then puts "a57"
  elsif x == 58 then puts "a58"
  elsif x == 59 then puts "a59"
  elsif x == 60 then puts "a60"
  elsif x == 61 then puts "a61"
  elsif x == 62 then puts "a62"
  elsif x == 63 then puts "a63"
  else
  if x == 64 then puts "a64"
  elsif x == 65 then puts "a65"
  elsif x == 66 then puts "a66"
  elsif chk([x, 67]) then puts "a67"
  elsif x == 68 then puts "a68"
  elsif x == 69 then puts "a69"
  elsif x == 70 then puts "a70"
  elsif x == 71 then puts "a71"
  elsif x == 72 then puts "a72"
  elsif x == 73 then puts "a73"
  elsif x == 74 then puts "a74"
  elsif x == 75 then puts "a75"
  elsif x == 76 then puts "a76"
  elsif chk([x, 77]) then puts "a77"
  elsif x == 78 then puts "a78"
  elsif x == 79 then puts "a79"
  elsif x == 80 then puts "a80"
  elsif x == 81 then puts "a81"
  elsif x == 82 then puts "a82"
  elsif x == 83 then puts "a83"
  elsif x == 84 then puts "a84"
  elsif x == 85 then puts "a85"
  elsif x == 86 then puts "a86"
  elsif chk([x, 87]) then puts "a87"
  elsif x == 88 then puts "a88"
  elsif x == 89 then puts "a89"
  elsif x == 90 then puts "a90"
  elsif x == 91 then puts "a91"
  elsif x == 92 then puts "a92"
  elsif x == 93 then puts "a93"
  elsif x == 94 then puts "a94"
  elsif x == 95 then puts "a95"
  elsif x == 96 then puts "a96"
  elsif chk([x, 97]) then puts "a97"
  elsif x == 98 then puts "a98"
  elsif x == 99 then puts "a99"
  elsif x == 100 then puts "a100"
  elsif x == 101 then puts "a101"
  elsif x == 102 then puts "a102"
  elsif x == 103 then puts "a103"
  elsif x == 104 then puts "a104"
  elsif x == 105 then puts "a105"
  elsif x == 106 then puts "a106"
  elsif chk([x, 107]) then puts "a107"
  elsif x == 108 then puts "a108"
  elsif x == 109 then puts "a109"
  elsif x == 110 then puts "a110"
  elsif x == 111 then puts "a111"
  elsif x == 112 then puts "a112"
  elsif x == 113 then puts "a113"
  elsif x == 114 then puts "a114"
  elsif x == 115 then puts "a115"
  elsif x == 116 then puts "a116"
  elsif chk([x, 117]) then puts "a117"
  elsif x == 118 then puts "a118"
  elsif x == 119 then puts "a119"
  elsif x == 120 then puts "a120"
  elsif x == 121 then puts "a121"
  elsif x == 122 then puts "a122"
  elsif x == 123 then puts "a123"
  elsif x == 124 then puts "a124"
  elsif x == 125 then puts "a125"
  elsif x == 126 then puts "a126"
  elsif chk([x, 127]) then puts "a127"
  else
  if x == 128 then puts "a128"
  elsif x == 129 then puts "a129"
  elsif x == 130 then puts "a130"
  elsif x == 131 then puts "a131"
  elsif x == 132 then puts "a132"
  elsif x == 133 then puts "a133"
  elsif x == 134 then puts "a134"
  elsif x == 135 then puts "a135"
  elsif x == 136 then puts "a136"
  elsif chk([x, 137]) then puts "a137"
  elsif x == 138 then puts "a138"
  elsif x == 139 then puts "a139"
  elsif x == 140 then puts "a140"
  elsif x == 141 then puts "a141"
  elsif x == 142 then puts "a142"
  elsif x == 143 then puts "a143"
  elsif x == 144 then puts "a144"
  elsif x == 145 then puts "a145"
  elsif x == 146 then puts "a146"
  elsif chk([x, 147]) then puts "a147"
  elsif x == 148 then puts "a148"
  elsif x == 149 then puts "a149"
  elsif x == 150 then puts "a150"
  elsif x == 151 then puts "a151"
  elsif x == 152 then puts "a152"
  elsif x == 153 then puts "a153"
  elsif x == 154 then puts "a154"
  elsif x == 155 then puts "a155"
  elsif x == 156 then puts "a156"
  elsif chk([x, 157]) then puts "a157"
  elsif x == 158 then puts "a158"
  elsif x == 159 then puts "a159"
  elsif x == 160 then puts "a160"
  elsif x == 161 then puts "a161"
  elsif x == 162 then puts "a162"
  elsif x == 163 then puts "a163"
  elsif x == 164 then puts "a164"
  elsif x == 165 then puts "a165"
  elsif x == 166 then puts "a166"
  elsif chk([x, 167]) then puts "a167"
  elsif x == 168 then puts "a168"
  elsif x == 169 then puts "a169"
  elsif x == 170 then puts "a170"
  elsif x == 171 then puts "a171"
  elsif x == 172 then puts "a172"
  elsif x == 173 then puts "a173"
  elsif x == 174 then puts "a174"
  elsif x == 175 then puts "a175"
  elsif x == 176 then puts "a176"
  elsif chk([x, 177]) then puts "a177"
  elsif x == 178 then puts "a178"
  elsif x == 179 then puts "a179"
  elsif x == 180 then puts "a180"
  elsif x == 181 then puts "a181"
  elsif x == 182 then puts "a182"
  elsif x == 183 then puts "a183"
  elsif x == 184 then puts "a184"
  elsif x == 185 then puts "a185"
  elsif x == 186 then puts "a186"
  elsif chk([x, 187]) then puts "a187"
  elsif x == 188 then puts "a188"
  elsif x == 189 then puts "a189"
  elsif x == 190 then puts "a190"
  elsif x == 191 then puts "a191"
  else
  unless x != 192
    puts "a192"
  else
  if x == 193 then puts "a193"
  elsif x == 194 then puts "a194"
  elsif x == 195 then puts "a195"
  elsif x == 196 then puts "a196"
  elsif chk([x, 197]) then puts "a197"
  elsif x == 198 then puts "a198"
  elsif x == 199 then puts "a199"
  elsif x == 200 then puts "a200"
  elsif x == 201 then puts "a201"
  elsif x == 202 then puts "a202"
  elsif x == 203 then puts "a203"
  elsif x == 204 then puts "a204"
  elsif x == 205 then puts "a205"
  elsif x == 206 then puts "a206"
  elsif chk([x, 207]) then puts "a207"
  elsif x == 208 then puts "a208"
  elsif x == 209 then puts "a209"
  elsif x == 210 then puts "a210"
  elsif x == 211 then puts "a211"
  elsif x == 212 then puts "a212"
  elsif x == 213 then puts "a213"
  elsif x == 214 then puts "a214"
  elsif x == 215 then puts "a215"
  elsif x == 216 then puts "a216"
  elsif chk([x, 217]) then puts "a217"
  elsif x == 218 then puts "a218"
  elsif x == 219 then puts "a219"
  elsif x == 220 then puts "a220"
  elsif x == 221 then puts "a221"
  elsif x == 222 then puts "a222"
  elsif x == 223 then puts "a223"
  elsif x == 224 then puts "a224"
  elsif x == 225 then puts "a225"
  elsif x == 226 then puts "a226"
  elsif chk([x, 227]) then puts "a227"
  elsif x == 228 then puts "a228"
  elsif x == 229 then puts "a229"
  elsif x == 230 then puts "a230"
  elsif x == 231 then puts "a231"
  elsif x == 232 then puts "a232"
  elsif x == 233 then puts "a233"
  elsif x == 234 then puts "a234"
  elsif x == 235 then puts "a235"
  elsif x == 236 then puts "a236"
  elsif chk([x, 237]) then puts "a237"
  elsif x == 238 then puts "a238"
  elsif x == 239 then puts "a239"
  elsif x == 240 then puts "a240"
  elsif x == 241 then puts "a241"
  elsif x == 242 then puts "a242"
  elsif x == 243 then puts "a243"
  elsif x == 244 then puts "a244"
  elsif x == 245 then puts "a245"
  elsif x == 246 then puts "a246"
  elsif chk([x, 247]) then puts "a247"
  elsif x == 248 then puts "a248"
  elsif x == 249 then puts "a249"
  elsif x == 250 then puts "a250"
  elsif x == 251 then puts "a251"
  elsif x == 252 then puts "a252"
  elsif x == 253 then puts "a253"
  elsif x == 254 then puts "a254"
  elsif x == 255 then puts "a255"
  else
  if x == 256 then puts "a256"
  elsif chk([x, 257]) then puts "a257"
  elsif x == 258 then puts "a258"
  elsif x == 259 then puts "a259"
  else puts "a_else"
  end
  end
  end
  end
  end
  end
end
xs.each { |x| $seen.clear; lad_a(x); p $seen.size }

def lad_b(x)
  y =
    if x == 0 then $log << :b0; 0 * 2
    elsif x == 1 then $log << :b1; 1 * 2
    elsif x == 2 then $log << :b2; 2 * 2
    elsif chk([x, 3]) then $log << :b3; 3 * 2
    elsif x == 4 then $log << :b4; 4 * 2
    elsif x == 5 then $log << :b5; 5 * 2
    elsif x == 6 then $log << :b6; 6 * 2
    elsif x == 7 then $log << :b7; 7 * 2
    elsif x == 8 then $log << :b8; 8 * 2
    elsif x == 9 then $log << :b9; 9 * 2
    elsif x == 10 then $log << :b10; 10 * 2
    elsif x == 11 then $log << :b11; 11 * 2
    elsif x == 12 then $log << :b12; 12 * 2
    elsif chk([x, 13]) then $log << :b13; 13 * 2
    elsif x == 14 then $log << :b14; 14 * 2
    elsif x == 15 then $log << :b15; 15 * 2
    elsif x == 16 then $log << :b16; 16 * 2
    elsif x == 17 then $log << :b17; 17 * 2
    elsif x == 18 then $log << :b18; 18 * 2
    elsif x == 19 then $log << :b19; 19 * 2
    elsif x == 20 then $log << :b20; 20 * 2
    elsif x == 21 then $log << :b21; 21 * 2
    elsif x == 22 then $log << :b22; 22 * 2
    elsif chk([x, 23]) then $log << :b23; 23 * 2
    elsif x == 24 then $log << :b24; 24 * 2
    elsif x == 25 then $log << :b25; 25 * 2
    elsif x == 26 then $log << :b26; 26 * 2
    elsif x == 27 then $log << :b27; 27 * 2
    elsif x == 28 then $log << :b28; 28 * 2
    elsif x == 29 then $log << :b29; 29 * 2
    elsif x == 30 then $log << :b30; 30 * 2
    elsif x == 31 then $log << :b31; 31 * 2
    elsif x == 32 then $log << :b32; 32 * 2
    elsif chk([x, 33]) then $log << :b33; 33 * 2
    elsif x == 34 then $log << :b34; 34 * 2
    elsif x == 35 then $log << :b35; 35 * 2
    elsif x == 36 then $log << :b36; 36 * 2
    elsif x == 37 then $log << :b37; 37 * 2
    elsif x == 38 then $log << :b38; 38 * 2
    elsif x == 39 then $log << :b39; 39 * 2
    elsif x == 40 then $log << :b40; 40 * 2
    elsif x == 41 then $log << :b41; 41 * 2
    elsif x == 42 then $log << :b42; 42 * 2
    elsif chk([x, 43]) then $log << :b43; 43 * 2
    elsif x == 44 then $log << :b44; 44 * 2
    elsif x == 45 then $log << :b45; 45 * 2
    elsif x == 46 then $log << :b46; 46 * 2
    elsif x == 47 then $log << :b47; 47 * 2
    elsif x == 48 then $log << :b48; 48 * 2
    elsif x == 49 then $log << :b49; 49 * 2
    elsif x == 50 then $log << :b50; 50 * 2
    elsif x == 51 then $log << :b51; 51 * 2
    elsif x == 52 then $log << :b52; 52 * 2
    elsif chk([x, 53]) then $log << :b53; 53 * 2
    elsif x == 54 then $log << :b54; 54 * 2
    elsif x == 55 then $log << :b55; 55 * 2
    elsif x == 56 then $log << :b56; 56 * 2
    elsif x == 57 then $log << :b57; 57 * 2
    elsif x == 58 then $log << :b58; 58 * 2
    elsif x == 59 then $log << :b59; 59 * 2
    elsif x == 60 then $log << :b60; 60 * 2
    elsif x == 61 then $log << :b61; 61 * 2
    elsif x == 62 then $log << :b62; 62 * 2
    elsif chk([x, 63]) then $log << :b63; 63 * 2
    else
    if x == 64 then $log << :b64; 64 * 2
    elsif x == 65 then $log << :b65; 65 * 2
    elsif x == 66 then $log << :b66; 66 * 2
    elsif x == 67 then $log << :b67; 67 * 2
    elsif x == 68 then $log << :b68; 68 * 2
    elsif x == 69 then $log << :b69; 69 * 2
    elsif x == 70 then $log << :b70; 70 * 2
    elsif x == 71 then $log << :b71; 71 * 2
    elsif x == 72 then $log << :b72; 72 * 2
    elsif chk([x, 73]) then $log << :b73; 73 * 2
    elsif x == 74 then $log << :b74; 74 * 2
    elsif x == 75 then $log << :b75; 75 * 2
    elsif x == 76 then $log << :b76; 76 * 2
    elsif x == 77 then $log << :b77; 77 * 2
    elsif x == 78 then $log << :b78; 78 * 2
    elsif x == 79 then $log << :b79; 79 * 2
    elsif x == 80 then $log << :b80; 80 * 2
    elsif x == 81 then $log << :b81; 81 * 2
    elsif x == 82 then $log << :b82; 82 * 2
    elsif chk([x, 83]) then $log << :b83; 83 * 2
    elsif x == 84 then $log << :b84; 84 * 2
    elsif x == 85 then $log << :b85; 85 * 2
    elsif x == 86 then $log << :b86; 86 * 2
    elsif x == 87 then $log << :b87; 87 * 2
    elsif x == 88 then $log << :b88; 88 * 2
    elsif x == 89 then $log << :b89; 89 * 2
    elsif x == 90 then $log << :b90; 90 * 2
    elsif x == 91 then $log << :b91; 91 * 2
    elsif x == 92 then $log << :b92; 92 * 2
    elsif chk([x, 93]) then $log << :b93; 93 * 2
    elsif x == 94 then $log << :b94; 94 * 2
    elsif x == 95 then $log << :b95; 95 * 2
    elsif x == 96 then $log << :b96; 96 * 2
    elsif x == 97 then $log << :b97; 97 * 2
    elsif x == 98 then $log << :b98; 98 * 2
    elsif x == 99 then $log << :b99; 99 * 2
    elsif x == 100 then $log << :b100; 100 * 2
    elsif x == 101 then $log << :b101; 101 * 2
    elsif x == 102 then $log << :b102; 102 * 2
    elsif chk([x, 103]) then $log << :b103; 103 * 2
    elsif x == 104 then $log << :b104; 104 * 2
    elsif x == 105 then $log << :b105; 105 * 2
    elsif x == 106 then $log << :b106; 106 * 2
    elsif x == 107 then $log << :b107; 107 * 2
    elsif x == 108 then $log << :b108; 108 * 2
    elsif x == 109 then $log << :b109; 109 * 2
    elsif x == 110 then $log << :b110; 110 * 2
    elsif x == 111 then $log << :b111; 111 * 2
    elsif x == 112 then $log << :b112; 112 * 2
    elsif chk([x, 113]) then $log << :b113; 113 * 2
    elsif x == 114 then $log << :b114; 114 * 2
    elsif x == 115 then $log << :b115; 115 * 2
    elsif x == 116 then $log << :b116; 116 * 2
    elsif x == 117 then $log << :b117; 117 * 2
    elsif x == 118 then $log << :b118; 118 * 2
    elsif x == 119 then $log << :b119; 119 * 2
    elsif x == 120 then $log << :b120; 120 * 2
    elsif x == 121 then $log << :b121; 121 * 2
    elsif x == 122 then $log << :b122; 122 * 2
    elsif chk([x, 123]) then $log << :b123; 123 * 2
    elsif x == 124 then $log << :b124; 124 * 2
    elsif x == 125 then $log << :b125; 125 * 2
    elsif x == 126 then $log << :b126; 126 * 2
    elsif x == 127 then $log << :b127; 127 * 2
    else
    unless x != 128
      $log << :b128; 128 * 2
    else
    if x == 129 then $log << :b129; 129 * 2
    elsif x == 130 then $log << :b130; 130 * 2
    elsif x == 131 then $log << :b131; 131 * 2
    elsif x == 132 then $log << :b132; 132 * 2
    elsif chk([x, 133]) then $log << :b133; 133 * 2
    elsif x == 134 then $log << :b134; 134 * 2
    elsif x == 135 then $log << :b135; 135 * 2
    elsif x == 136 then $log << :b136; 136 * 2
    elsif x == 137 then $log << :b137; 137 * 2
    elsif x == 138 then $log << :b138; 138 * 2
    elsif x == 139 then $log << :b139; 139 * 2
    elsif x == 140 then $log << :b140; 140 * 2
    elsif x == 141 then $log << :b141; 141 * 2
    elsif x == 142 then $log << :b142; 142 * 2
    elsif chk([x, 143]) then $log << :b143; 143 * 2
    elsif x == 144 then $log << :b144; 144 * 2
    elsif x == 145 then $log << :b145; 145 * 2
    elsif x == 146 then $log << :b146; 146 * 2
    elsif x == 147 then $log << :b147; 147 * 2
    elsif x == 148 then $log << :b148; 148 * 2
    elsif x == 149 then $log << :b149; 149 * 2
    elsif x == 150 then $log << :b150; 150 * 2
    elsif x == 151 then $log << :b151; 151 * 2
    elsif x == 152 then $log << :b152; 152 * 2
    elsif chk([x, 153]) then $log << :b153; 153 * 2
    elsif x == 154 then $log << :b154; 154 * 2
    elsif x == 155 then $log << :b155; 155 * 2
    elsif x == 156 then $log << :b156; 156 * 2
    elsif x == 157 then $log << :b157; 157 * 2
    elsif x == 158 then $log << :b158; 158 * 2
    elsif x == 159 then $log << :b159; 159 * 2
    elsif x == 160 then $log << :b160; 160 * 2
    elsif x == 161 then $log << :b161; 161 * 2
    elsif x == 162 then $log << :b162; 162 * 2
    elsif chk([x, 163]) then $log << :b163; 163 * 2
    elsif x == 164 then $log << :b164; 164 * 2
    elsif x == 165 then $log << :b165; 165 * 2
    elsif x == 166 then $log << :b166; 166 * 2
    elsif x == 167 then $log << :b167; 167 * 2
    elsif x == 168 then $log << :b168; 168 * 2
    elsif x == 169 then $log << :b169; 169 * 2
    elsif x == 170 then $log << :b170; 170 * 2
    elsif x == 171 then $log << :b171; 171 * 2
    elsif x == 172 then $log << :b172; 172 * 2
    elsif chk([x, 173]) then $log << :b173; 173 * 2
    elsif x == 174 then $log << :b174; 174 * 2
    elsif x == 175 then $log << :b175; 175 * 2
    elsif x == 176 then $log << :b176; 176 * 2
    elsif x == 177 then $log << :b177; 177 * 2
    elsif x == 178 then $log << :b178; 178 * 2
    elsif x == 179 then $log << :b179; 179 * 2
    elsif x == 180 then $log << :b180; 180 * 2
    elsif x == 181 then $log << :b181; 181 * 2
    elsif x == 182 then $log << :b182; 182 * 2
    elsif chk([x, 183]) then $log << :b183; 183 * 2
    elsif x == 184 then $log << :b184; 184 * 2
    elsif x == 185 then $log << :b185; 185 * 2
    elsif x == 186 then $log << :b186; 186 * 2
    elsif x == 187 then $log << :b187; 187 * 2
    elsif x == 188 then $log << :b188; 188 * 2
    elsif x == 189 then $log << :b189; 189 * 2
    elsif x == 190 then $log << :b190; 190 * 2
    elsif x == 191 then $log << :b191; 191 * 2
    else
    if x == 192 then $log << :b192; 192 * 2
    elsif chk([x, 193]) then $log << :b193; 193 * 2
    elsif x == 194 then $log << :b194; 194 * 2
    elsif x == 195 then $log << :b195; 195 * 2
    elsif x == 196 then $log << :b196; 196 * 2
    elsif x == 197 then $log << :b197; 197 * 2
    elsif x == 198 then $log << :b198; 198 * 2
    elsif x == 199 then $log << :b199; 199 * 2
    elsif x == 200 then $log << :b200; 200 * 2
    elsif x == 201 then $log << :b201; 201 * 2
    elsif x == 202 then $log << :b202; 202 * 2
    elsif chk([x, 203]) then $log << :b203; 203 * 2
    elsif x == 204 then $log << :b204; 204 * 2
    elsif x == 205 then $log << :b205; 205 * 2
    elsif x == 206 then $log << :b206; 206 * 2
    elsif x == 207 then $log << :b207; 207 * 2
    elsif x == 208 then $log << :b208; 208 * 2
    elsif x == 209 then $log << :b209; 209 * 2
    elsif x == 210 then $log << :b210; 210 * 2
    elsif x == 211 then $log << :b211; 211 * 2
    elsif x == 212 then $log << :b212; 212 * 2
    elsif chk([x, 213]) then $log << :b213; 213 * 2
    elsif x == 214 then $log << :b214; 214 * 2
    elsif x == 215 then $log << :b215; 215 * 2
    elsif x == 216 then $log << :b216; 216 * 2
    elsif x == 217 then $log << :b217; 217 * 2
    elsif x == 218 then $log << :b218; 218 * 2
    elsif x == 219 then $log << :b219; 219 * 2
    elsif x == 220 then $log << :b220; 220 * 2
    elsif x == 221 then $log << :b221; 221 * 2
    elsif x == 222 then $log << :b222; 222 * 2
    elsif chk([x, 223]) then $log << :b223; 223 * 2
    elsif x == 224 then $log << :b224; 224 * 2
    elsif x == 225 then $log << :b225; 225 * 2
    elsif x == 226 then $log << :b226; 226 * 2
    elsif x == 227 then $log << :b227; 227 * 2
    elsif x == 228 then $log << :b228; 228 * 2
    elsif x == 229 then $log << :b229; 229 * 2
    elsif x == 230 then $log << :b230; 230 * 2
    elsif x == 231 then $log << :b231; 231 * 2
    elsif x == 232 then $log << :b232; 232 * 2
    elsif chk([x, 233]) then $log << :b233; 233 * 2
    elsif x == 234 then $log << :b234; 234 * 2
    elsif x == 235 then $log << :b235; 235 * 2
    elsif x == 236 then $log << :b236; 236 * 2
    elsif x == 237 then $log << :b237; 237 * 2
    elsif x == 238 then $log << :b238; 238 * 2
    elsif x == 239 then $log << :b239; 239 * 2
    elsif x == 240 then $log << :b240; 240 * 2
    elsif x == 241 then $log << :b241; 241 * 2
    elsif x == 242 then $log << :b242; 242 * 2
    elsif chk([x, 243]) then $log << :b243; 243 * 2
    elsif x == 244 then $log << :b244; 244 * 2
    elsif x == 245 then $log << :b245; 245 * 2
    elsif x == 246 then $log << :b246; 246 * 2
    elsif x == 247 then $log << :b247; 247 * 2
    elsif x == 248 then $log << :b248; 248 * 2
    elsif x == 249 then $log << :b249; 249 * 2
    elsif x == 250 then $log << :b250; 250 * 2
    elsif x == 251 then $log << :b251; 251 * 2
    elsif x == 252 then $log << :b252; 252 * 2
    elsif chk([x, 253]) then $log << :b253; 253 * 2
    elsif x == 254 then $log << :b254; 254 * 2
    elsif x == 255 then $log << :b255; 255 * 2
    else
    if x == 256 then $log << :b256; 256 * 2
    elsif x == 257 then $log << :b257; 257 * 2
    elsif x == 258 then $log << :b258; 258 * 2
    elsif x == 259 then $log << :b259; 259 * 2
    else $log << :b_else; -1
    end
    end
    end
    end
    end
    end
  y
end
xs.each do |x|
  $seen.clear; $log.clear
  r = lad_b(x)
  p [r, $seen.size, $log.size, $log.last]
end

p(xs.map { |x| x == 0 ? 0 : x == 1 ? 1 : x == 2 ? 2 : x == 3 ? 3 : x == 4 ? 4 : x == 5 ? 5 : x == 6 ? 6 : x == 7 ? 7 : x == 8 ? 8 : x == 9 ? 9 : x == 10 ? 10 : x == 11 ? 11 : x == 12 ? 12 : x == 13 ? 13 : x == 14 ? 14 : x == 15 ? 15 : x == 16 ? 16 : x == 17 ? 17 : x == 18 ? 18 : x == 19 ? 19 : x == 20 ? 20 : x == 21 ? 21 : x == 22 ? 22 : x == 23 ? 23 : x == 24 ? 24 : x == 25 ? 25 : x == 26 ? 26 : x == 27 ? 27 : x == 28 ? 28 : x == 29 ? 29 : x == 30 ? 30 : x == 31 ? 31 : x == 32 ? 32 : x == 33 ? 33 : x == 34 ? 34 : x == 35 ? 35 : x == 36 ? 36 : x == 37 ? 37 : x == 38 ? 38 : x == 39 ? 39 : x == 40 ? 40 : x == 41 ? 41 : x == 42 ? 42 : x == 43 ? 43 : x == 44 ? 44 : x == 45 ? 45 : x == 46 ? 46 : x == 47 ? 47 : x == 48 ? 48 : x == 49 ? 49 : x == 50 ? 50 : x == 51 ? 51 : x == 52 ? 52 : x == 53 ? 53 : x == 54 ? 54 : x == 55 ? 55 : x == 56 ? 56 : x == 57 ? 57 : x == 58 ? 58 : x == 59 ? 59 : x == 60 ? 60 : x == 61 ? 61 : x == 62 ? 62 : x == 63 ? 63 : x == 64 ? 64 : x == 65 ? 65 : x == 66 ? 66 : x == 67 ? 67 : x == 68 ? 68 : x == 69 ? 69 : x == 70 ? 70 : x == 71 ? 71 : x == 72 ? 72 : x == 73 ? 73 : x == 74 ? 74 : x == 75 ? 75 : x == 76 ? 76 : x == 77 ? 77 : x == 78 ? 78 : x == 79 ? 79 : x == 80 ? 80 : x == 81 ? 81 : x == 82 ? 82 : x == 83 ? 83 : x == 84 ? 84 : x == 85 ? 85 : x == 86 ? 86 : x == 87 ? 87 : x == 88 ? 88 : x == 89 ? 89 : x == 90 ? 90 : x == 91 ? 91 : x == 92 ? 92 : x == 93 ? 93 : x == 94 ? 94 : x == 95 ? 95 : x == 96 ? 96 : x == 97 ? 97 : x == 98 ? 98 : x == 99 ? 99 : x == 100 ? 100 : x == 101 ? 101 : x == 102 ? 102 : x == 103 ? 103 : x == 104 ? 104 : x == 105 ? 105 : x == 106 ? 106 : x == 107 ? 107 : x == 108 ? 108 : x == 109 ? 109 : x == 110 ? 110 : x == 111 ? 111 : x == 112 ? 112 : x == 113 ? 113 : x == 114 ? 114 : x == 115 ? 115 : x == 116 ? 116 : x == 117 ? 117 : x == 118 ? 118 : x == 119 ? 119 : x == 120 ? 120 : x == 121 ? 121 : x == 122 ? 122 : x == 123 ? 123 : x == 124 ? 124 : x == 125 ? 125 : x == 126 ? 126 : x == 127 ? 127 : x == 128 ? 128 : x == 129 ? 129 : x == 130 ? 130 : x == 131 ? 131 : x == 132 ? 132 : x == 133 ? 133 : x == 134 ? 134 : x == 135 ? 135 : x == 136 ? 136 : x == 137 ? 137 : x == 138 ? 138 : x == 139 ? 139 : x == 140 ? 140 : x == 141 ? 141 : x == 142 ? 142 : x == 143 ? 143 : x == 144 ? 144 : x == 145 ? 145 : x == 146 ? 146 : x == 147 ? 147 : x == 148 ? 148 : x == 149 ? 149 : x == 150 ? 150 : x == 151 ? 151 : x == 152 ? 152 : x == 153 ? 153 : x == 154 ? 154 : x == 155 ? 155 : x == 156 ? 156 : x == 157 ? 157 : x == 158 ? 158 : x == 159 ? 159 : x == 160 ? 160 : x == 161 ? 161 : x == 162 ? 162 : x == 163 ? 163 : x == 164 ? 164 : x == 165 ? 165 : x == 166 ? 166 : x == 167 ? 167 : x == 168 ? 168 : x == 169 ? 169 : x == 170 ? 170 : x == 171 ? 171 : x == 172 ? 172 : x == 173 ? 173 : x == 174 ? 174 : x == 175 ? 175 : x == 176 ? 176 : x == 177 ? 177 : x == 178 ? 178 : x == 179 ? 179 : x == 180 ? 180 : x == 181 ? 181 : x == 182 ? 182 : x == 183 ? 183 : x == 184 ? 184 : x == 185 ? 185 : x == 186 ? 186 : x == 187 ? 187 : x == 188 ? 188 : x == 189 ? 189 : x == 190 ? 190 : x == 191 ? 191 : x == 192 ? 192 : x == 193 ? 193 : x == 194 ? 194 : x == 195 ? 195 : x == 196 ? 196 : x == 197 ? 197 : x == 198 ? 198 : x == 199 ? 199 : x == 200 ? 200 : x == 201 ? 201 : x == 202 ? 202 : x == 203 ? 203 : x == 204 ? 204 : x == 205 ? 205 : x == 206 ? 206 : x == 207 ? 207 : x == 208 ? 208 : x == 209 ? 209 : x == 210 ? 210 : x == 211 ? 211 : x == 212 ? 212 : x == 213 ? 213 : x == 214 ? 214 : x == 215 ? 215 : x == 216 ? 216 : x == 217 ? 217 : x == 218 ? 218 : x == 219 ? 219 : x == 220 ? 220 : x == 221 ? 221 : x == 222 ? 222 : x == 223 ? 223 : x == 224 ? 224 : x == 225 ? 225 : x == 226 ? 226 : x == 227 ? 227 : x == 228 ? 228 : x == 229 ? 229 : x == 230 ? 230 : x == 231 ? 231 : x == 232 ? 232 : x == 233 ? 233 : x == 234 ? 234 : x == 235 ? 235 : x == 236 ? 236 : x == 237 ? 237 : x == 238 ? 238 : x == 239 ? 239 : x == 240 ? 240 : x == 241 ? 241 : x == 242 ? 242 : x == 243 ? 243 : x == 244 ? 244 : x == 245 ? 245 : x == 246 ? 246 : x == 247 ? 247 : x == 248 ? 248 : x == 249 ? 249 : x == 250 ? 250 : x == 251 ? 251 : x == 252 ? 252 : x == 253 ? 253 : x == 254 ? 254 : x == 255 ? 255 : x == 256 ? 256 : x == 257 ? 257 : x == 258 ? 258 : x == 259 ? 259 : -1 })
xs.each do |x|
  $seen.clear; $log.clear
  r = chk([g(x), 0]) ? "t0" : chk([g(x), 1]) ? "t1" : chk([g(x), 2]) ? "t2" : chk([g(x), 3]) ? "t3" : chk([g(x), 4]) ? "t4" : chk([g(x), 5]) ? "t5" : chk([g(x), 6]) ? "t6" : chk([g(x), 7]) ? "t7" : chk([g(x), 8]) ? "t8" : chk([g(x), 9]) ? "t9" : chk([g(x), 10]) ? "t10" : chk([g(x), 11]) ? "t11" : chk([g(x), 12]) ? "t12" : chk([g(x), 13]) ? "t13" : chk([g(x), 14]) ? "t14" : chk([g(x), 15]) ? "t15" : chk([g(x), 16]) ? "t16" : chk([g(x), 17]) ? "t17" : chk([g(x), 18]) ? "t18" : chk([g(x), 19]) ? "t19" : chk([g(x), 20]) ? "t20" : chk([g(x), 21]) ? "t21" : chk([g(x), 22]) ? "t22" : chk([g(x), 23]) ? "t23" : chk([g(x), 24]) ? "t24" : chk([g(x), 25]) ? "t25" : chk([g(x), 26]) ? "t26" : chk([g(x), 27]) ? "t27" : chk([g(x), 28]) ? "t28" : chk([g(x), 29]) ? "t29" : chk([g(x), 30]) ? "t30" : chk([g(x), 31]) ? "t31" : chk([g(x), 32]) ? "t32" : chk([g(x), 33]) ? "t33" : chk([g(x), 34]) ? "t34" : chk([g(x), 35]) ? "t35" : chk([g(x), 36]) ? "t36" : chk([g(x), 37]) ? "t37" : chk([g(x), 38]) ? "t38" : chk([g(x), 39]) ? "t39" : chk([g(x), 40]) ? "t40" : chk([g(x), 41]) ? "t41" : chk([g(x), 42]) ? "t42" : chk([g(x), 43]) ? "t43" : chk([g(x), 44]) ? "t44" : chk([g(x), 45]) ? "t45" : chk([g(x), 46]) ? "t46" : chk([g(x), 47]) ? "t47" : chk([g(x), 48]) ? "t48" : chk([g(x), 49]) ? "t49" : chk([g(x), 50]) ? "t50" : chk([g(x), 51]) ? "t51" : chk([g(x), 52]) ? "t52" : chk([g(x), 53]) ? "t53" : chk([g(x), 54]) ? "t54" : chk([g(x), 55]) ? "t55" : chk([g(x), 56]) ? "t56" : chk([g(x), 57]) ? "t57" : chk([g(x), 58]) ? "t58" : chk([g(x), 59]) ? "t59" : chk([g(x), 60]) ? "t60" : chk([g(x), 61]) ? "t61" : chk([g(x), 62]) ? "t62" : chk([g(x), 63]) ? "t63" : chk([g(x), 64]) ? "t64" : chk([g(x), 65]) ? "t65" : chk([g(x), 66]) ? "t66" : chk([g(x), 67]) ? "t67" : chk([g(x), 68]) ? "t68" : chk([g(x), 69]) ? "t69" : chk([g(x), 70]) ? "t70" : chk([g(x), 71]) ? "t71" : chk([g(x), 72]) ? "t72" : chk([g(x), 73]) ? "t73" : chk([g(x), 74]) ? "t74" : chk([g(x), 75]) ? "t75" : chk([g(x), 76]) ? "t76" : chk([g(x), 77]) ? "t77" : chk([g(x), 78]) ? "t78" : chk([g(x), 79]) ? "t79" : chk([g(x), 80]) ? "t80" : chk([g(x), 81]) ? "t81" : chk([g(x), 82]) ? "t82" : chk([g(x), 83]) ? "t83" : chk([g(x), 84]) ? "t84" : chk([g(x), 85]) ? "t85" : chk([g(x), 86]) ? "t86" : chk([g(x), 87]) ? "t87" : chk([g(x), 88]) ? "t88" : chk([g(x), 89]) ? "t89" : chk([g(x), 90]) ? "t90" : chk([g(x), 91]) ? "t91" : chk([g(x), 92]) ? "t92" : chk([g(x), 93]) ? "t93" : chk([g(x), 94]) ? "t94" : chk([g(x), 95]) ? "t95" : chk([g(x), 96]) ? "t96" : chk([g(x), 97]) ? "t97" : chk([g(x), 98]) ? "t98" : chk([g(x), 99]) ? "t99" : chk([g(x), 100]) ? "t100" : chk([g(x), 101]) ? "t101" : chk([g(x), 102]) ? "t102" : chk([g(x), 103]) ? "t103" : chk([g(x), 104]) ? "t104" : chk([g(x), 105]) ? "t105" : chk([g(x), 106]) ? "t106" : chk([g(x), 107]) ? "t107" : chk([g(x), 108]) ? "t108" : chk([g(x), 109]) ? "t109" : chk([g(x), 110]) ? "t110" : chk([g(x), 111]) ? "t111" : chk([g(x), 112]) ? "t112" : chk([g(x), 113]) ? "t113" : chk([g(x), 114]) ? "t114" : chk([g(x), 115]) ? "t115" : chk([g(x), 116]) ? "t116" : chk([g(x), 117]) ? "t117" : chk([g(x), 118]) ? "t118" : chk([g(x), 119]) ? "t119" : chk([g(x), 120]) ? "t120" : chk([g(x), 121]) ? "t121" : chk([g(x), 122]) ? "t122" : chk([g(x), 123]) ? "t123" : chk([g(x), 124]) ? "t124" : chk([g(x), 125]) ? "t125" : chk([g(x), 126]) ? "t126" : chk([g(x), 127]) ? "t127" : chk([g(x), 128]) ? "t128" : chk([g(x), 129]) ? "t129" : chk([g(x), 130]) ? "t130" : chk([g(x), 131]) ? "t131" : chk([g(x), 132]) ? "t132" : chk([g(x), 133]) ? "t133" : chk([g(x), 134]) ? "t134" : chk([g(x), 135]) ? "t135" : chk([g(x), 136]) ? "t136" : chk([g(x), 137]) ? "t137" : chk([g(x), 138]) ? "t138" : chk([g(x), 139]) ? "t139" : chk([g(x), 140]) ? "t140" : chk([g(x), 141]) ? "t141" : chk([g(x), 142]) ? "t142" : chk([g(x), 143]) ? "t143" : chk([g(x), 144]) ? "t144" : chk([g(x), 145]) ? "t145" : chk([g(x), 146]) ? "t146" : chk([g(x), 147]) ? "t147" : chk([g(x), 148]) ? "t148" : chk([g(x), 149]) ? "t149" : chk([g(x), 150]) ? "t150" : chk([g(x), 151]) ? "t151" : chk([g(x), 152]) ? "t152" : chk([g(x), 153]) ? "t153" : chk([g(x), 154]) ? "t154" : chk([g(x), 155]) ? "t155" : chk([g(x), 156]) ? "t156" : chk([g(x), 157]) ? "t157" : chk([g(x), 158]) ? "t158" : chk([g(x), 159]) ? "t159" : chk([g(x), 160]) ? "t160" : chk([g(x), 161]) ? "t161" : chk([g(x), 162]) ? "t162" : chk([g(x), 163]) ? "t163" : chk([g(x), 164]) ? "t164" : chk([g(x), 165]) ? "t165" : chk([g(x), 166]) ? "t166" : chk([g(x), 167]) ? "t167" : chk([g(x), 168]) ? "t168" : chk([g(x), 169]) ? "t169" : chk([g(x), 170]) ? "t170" : chk([g(x), 171]) ? "t171" : chk([g(x), 172]) ? "t172" : chk([g(x), 173]) ? "t173" : chk([g(x), 174]) ? "t174" : chk([g(x), 175]) ? "t175" : chk([g(x), 176]) ? "t176" : chk([g(x), 177]) ? "t177" : chk([g(x), 178]) ? "t178" : chk([g(x), 179]) ? "t179" : chk([g(x), 180]) ? "t180" : chk([g(x), 181]) ? "t181" : chk([g(x), 182]) ? "t182" : chk([g(x), 183]) ? "t183" : chk([g(x), 184]) ? "t184" : chk([g(x), 185]) ? "t185" : chk([g(x), 186]) ? "t186" : chk([g(x), 187]) ? "t187" : chk([g(x), 188]) ? "t188" : chk([g(x), 189]) ? "t189" : chk([g(x), 190]) ? "t190" : chk([g(x), 191]) ? "t191" : chk([g(x), 192]) ? "t192" : chk([g(x), 193]) ? "t193" : chk([g(x), 194]) ? "t194" : chk([g(x), 195]) ? "t195" : chk([g(x), 196]) ? "t196" : chk([g(x), 197]) ? "t197" : chk([g(x), 198]) ? "t198" : chk([g(x), 199]) ? "t199" : chk([g(x), 200]) ? "t200" : chk([g(x), 201]) ? "t201" : chk([g(x), 202]) ? "t202" : chk([g(x), 203]) ? "t203" : chk([g(x), 204]) ? "t204" : chk([g(x), 205]) ? "t205" : chk([g(x), 206]) ? "t206" : chk([g(x), 207]) ? "t207" : chk([g(x), 208]) ? "t208" : chk([g(x), 209]) ? "t209" : chk([g(x), 210]) ? "t210" : chk([g(x), 211]) ? "t211" : chk([g(x), 212]) ? "t212" : chk([g(x), 213]) ? "t213" : chk([g(x), 214]) ? "t214" : chk([g(x), 215]) ? "t215" : chk([g(x), 216]) ? "t216" : chk([g(x), 217]) ? "t217" : chk([g(x), 218]) ? "t218" : chk([g(x), 219]) ? "t219" : chk([g(x), 220]) ? "t220" : chk([g(x), 221]) ? "t221" : chk([g(x), 222]) ? "t222" : chk([g(x), 223]) ? "t223" : chk([g(x), 224]) ? "t224" : chk([g(x), 225]) ? "t225" : chk([g(x), 226]) ? "t226" : chk([g(x), 227]) ? "t227" : chk([g(x), 228]) ? "t228" : chk([g(x), 229]) ? "t229" : chk([g(x), 230]) ? "t230" : chk([g(x), 231]) ? "t231" : chk([g(x), 232]) ? "t232" : chk([g(x), 233]) ? "t233" : chk([g(x), 234]) ? "t234" : chk([g(x), 235]) ? "t235" : chk([g(x), 236]) ? "t236" : chk([g(x), 237]) ? "t237" : chk([g(x), 238]) ? "t238" : chk([g(x), 239]) ? "t239" : chk([g(x), 240]) ? "t240" : chk([g(x), 241]) ? "t241" : chk([g(x), 242]) ? "t242" : chk([g(x), 243]) ? "t243" : chk([g(x), 244]) ? "t244" : chk([g(x), 245]) ? "t245" : chk([g(x), 246]) ? "t246" : chk([g(x), 247]) ? "t247" : chk([g(x), 248]) ? "t248" : chk([g(x), 249]) ? "t249" : chk([g(x), 250]) ? "t250" : chk([g(x), 251]) ? "t251" : chk([g(x), 252]) ? "t252" : chk([g(x), 253]) ? "t253" : chk([g(x), 254]) ? "t254" : chk([g(x), 255]) ? "t255" : chk([g(x), 256]) ? "t256" : chk([g(x), 257]) ? "t257" : chk([g(x), 258]) ? "t258" : chk([g(x), 259]) ? "t259" : "none"
  p [r, $log.size]
end

# short shapes
ys = [0, 1, 2, 3, 9]

# a method's tail, a block's value, a lambda's value
def tail(x)
  if x == 0 then 0 * 3
  elsif x == 1 then 1 * 3
  elsif x == 2 then 2 * 3
  else -1
  end
end
p ys.map { |x| tail(x) }
p(ys.map do |x|
  if x == 0 then :b0
  elsif x == 1 then :b1
  elsif x == 2 then :b2
  else :e
  end
end)
l = ->(x) {
  if x == 0 then :s0
  elsif x == 1 then :s1
  elsif x == 2 then :s2
  else :none
  end
}
p ys.map { |x| l.(x) }

# arms the compiler decides: a defined? of a missing constant, an is_a? it settles
def fold(x)
  if x == 0 then 0
  elsif defined?(NoSuchConst3) then -3
  elsif x.is_a?(String) then -5
  elsif x == 1 then 1
  elsif x == 2 then 2
  else :e
  end
end
p ys.map { |x| fold(x) }
def fold2(x)
  y = if x == 0 then 0
  elsif defined?(NoSuchConst4) then -4
  elsif x.is_a?(Integer) then :int
  elsif x == 1 then 1
  else :e
  end
  y
end
p ys.map { |x| fold2(x) }

# arms of five classes (a boxed result), an Integer arm beside nil, no else
p(ys.map do |x|
  if x == 0 then 0
  elsif x == 1 then "s1"
  elsif x == 2 then 2.5
  elsif x == 3 then nil
  else [x]
  end
end)
ys.each do |x|
  y =
    if x == 0 then 0
    elsif x == 1 then 1
    elsif x == 2 then nil
    else x
    end
  p [y, y.nil? ? :nil : y + 1]
end
ys.each do |x|
  y =
    if x == 0 then "v0"
    elsif x == 1 then "v1"
    elsif x == 2 then "v2"
    end
  p y
end

# a mutable String arm
ys.each do |x|
  y =
    if x == 0 then +"m0"
    elsif x == 1 then +"m1"
    elsif x == 2 then +"m2"
    else x.to_s
    end
  y << "!"
  p y
end

# unless arms, a diverging arm, and a ladder inside an arm
def small(x)
  r = if x == 0 then :zero
      elsif x == 1
        :one
      else
        unless x == 2
          :not_two
        else
          :two
        end
      end
  s = x == 5 ? (raise "five" rescue :rescued) : x == 6 ? :six : x == 7 ? :seven : :other
  [r, s]
end
p (0..7).map { |x| small(x) }
def inner(x)
  if x > 0
    if x == 1 then :a elsif x == 2 then :b else :c end
  elsif x < -5 then :low
  else
    :zero
  end
end
p [3, 2, 1, 0, -9].map { |x| inner(x) }

# return and next in an elsif or hand-written else + if arm, and break in an elsif arm, of a value ladder
def rv(x)
  v = if x == 0 then 0
      elsif x == 1 then return :one
      else
        if x == 2 then return :two else 3 end
      end
  [v]
end
p [0, 1, 2, 3].map { |x| rv(x) }
p([0, 1, 2, 3].map do |x|
  v = if x == 0 then :a
      elsif x == 1 then next :n
      else
        if x == 2 then next :m else :z end
      end
  [v]
end)
p([0, 1, 2, 3].each do |x|
  v = if x == 0 then :a
      elsif x == 1 then break :b
      else :z
      end
end)

# levels of another type than the head: a ladder is flat only where its
# levels answer one type (a raise arm, a nil arm and a Hash beside a String)
def mixt(x) = x == 0 ? "a" : x == 1 ? 2 : x == 2 ? 3 : x == 3 ? nil : 5
p ys.map { |x| mixt(x) }
def mixr(x) = x == 0 ? [1] : x == 1 ? raise("boom") : x == 2 ? [3] : x == 3 ? raise(ArgumentError, "bad") : [5]
p((0..4).map { |x| (mixr(x) rescue $!.message) })
def mixv(x)
  if x == 0 then {"a" => 1}
  elsif x == 1 then (puts "w"; {"b" => 2})
  elsif x == 2 then nil
  else {"c" => 3} end
end
p (0..3).map { |x| mixv(x) }

# conditions with effects are tested in order, and only as far as the ladder goes
$log = []
def t(k, x) = ($log << k; x == k)
def multi(x)
  if t(0, x) then $log << :a0; 0
  elsif t(1, x) then $log << :a1; 1 * 2
  elsif t(2, x) then $log << :a2; 2 * 2
  else $log << :e; -1
  end
end
ys.each do |x|
  $log.clear
  r = multi(x)
  p [r, $log.size, $log.last(2)]
end

