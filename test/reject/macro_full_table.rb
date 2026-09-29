# A write the walk cannot follow, made when the table of known ivars is
# full: nothing is known after it, so the macro reading the state is left as
# written (a run-time public_send, refused), not given nil.
module Consts
  def kind(k)
    @kind = k
  end
  def constant(c)
    const_set(c, public_send(["calc", @kind, c.to_s.downcase].compact.join("_")))
  end
end
class Calc
  extend Consts
  def self.calc_size = 1
  def self.calc_box_size = 2
  @v1 = 1
  @v2 = 2
  @v3 = 3
  @v4 = 4
  @v5 = 5
  @v6 = 6
  @v7 = 7
  @v8 = 8
  @v9 = 9
  @v10 = 10
  @v11 = 11
  @v12 = 12
  @v13 = 13
  @v14 = 14
  @v15 = 15
  @v16 = 16
  @v17 = 17
  @v18 = 18
  @v19 = 19
  @v20 = 20
  @v21 = 21
  @v22 = 22
  @v23 = 23
  @v24 = 24
  @v25 = 25
  @v26 = 26
  @v27 = 27
  @v28 = 28
  @v29 = 29
  @v30 = 30
  @v31 = 31
  @v32 = 32
  @v33 = 33
  @v34 = 34
  @v35 = 35
  @v36 = 36
  @v37 = 37
  @v38 = 38
  @v39 = 39
  @v40 = 40
  @v41 = 41
  @v42 = 42
  @v43 = 43
  @v44 = 44
  @v45 = 45
  @v46 = 46
  @v47 = 47
  @v48 = 48
  @v49 = 49
  @v50 = 50
  @v51 = 51
  @v52 = 52
  @v53 = 53
  @v54 = 54
  @v55 = 55
  @v56 = 56
  @v57 = 57
  @v58 = 58
  @v59 = 59
  @v60 = 60
  @v61 = 61
  @v62 = 62
  @v63 = 63
  @v64 = 64
  @kind = :box if ARGV.empty?
  constant :SIZE
end
p Calc::SIZE
