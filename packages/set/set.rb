# Spinel bundled `set`.
#
# A Set keeps its members in an Array, in insertion order. Elements are
# unique by #hash and #eql? as in CRuby's Set -- a value object that defines
# #eql?/#hash is deduplicated and found by #include? even across distinct
# instances, and 1 and 1.0 are distinct members (1.eql?(1.0) is false). Past
# eight members a Hash beside the Array, whose keys are the members, makes a
# lookup one probe; up to eight the Set compares hashes along the Array, as
# CRuby's Hash keeps a small table flat, since a table costs more than it
# saves there. An identity Set walks its members with #equal? instead. This
# covers the standard Set surface (construction, iteration, membership, set
# algebra, comparison, classify/divide/flatten) rather than every CRuby
# corner.

class Set
  include Enumerable

  # A Set may hold itself (`s.add(s)`), directly or through an Array, a Hash or
  # any object that leads back to it, and #==, #inspect and #flatten all walk
  # their elements. Without a memory of what they are already inside, each of
  # them follows the cycle until the C stack runs out. The runtime keeps that
  # memory for Array and Hash -- a per-fiber path of the objects (or object
  # pairs) the current walk is inside, restored by every non-local exit -- and
  # a Set's walks, which are written in Ruby, join it through these bindings.
  # Each `enter` answers the mark that `leave` takes back, or -1 when the walk
  # is already inside this Set, which is the cue to stop. No `ensure` is
  # needed: a raise, throw or break out of a walk lands on a handler that
  # restores the path to the depth it had when that handler was armed.
  module RecursionGuard
    native_func :enter_inspect, [:any], :int, "sp_poly_recur_enter_inspect"
    native_func :enter_eq, [:any, :any], :int, "sp_poly_recur_enter_eq"
    native_func :enter_flatten, [:any], :int, "sp_poly_recur_enter_flatten"
    native_func :leave, [:int], :nil, "sp_poly_recur_leave"
  end

  # Set[1, 2, 3] builds a Set from the given elements (CRuby's Set.[]).
  def self.[](*args)
    new(args)
  end

  def initialize(enum = nil)
    @data = []
    @index = nil
    @ident = false
    if enum
      Set.check_enum(enum)
      # materialize first: the operand may be an Enumerator (or any other
      # Enumerable), whose #each is not reachable through a poly slot (#3625)
      src = enum.to_a
      if block_given?
        src.each { |x| add(yield(x)) }
      else
        src.each { |x| add(x) }
      end
    end
  end

  # dup and clone must not share the element array with the original. A copy
  # compares its elements as the original does.
  def initialize_copy(orig)
    adopt(orig.to_a, orig.compare_by_identity?)
  end

  # compare_by_identity makes the Set find its elements by equal? rather than
  # by eql? and hash, as CRuby's does. Spinel's identity is CRuby's for an
  # object, an Array, a Hash, a Symbol, nil, true and false; a program that
  # may put another kind into a Set beside an identity one is refused at
  # compile time.
  def compare_by_identity
    return self if @ident
    check_frozen
    @ident = true
    @index = nil
    self
  end

  def compare_by_identity?
    @ident
  end

  # The members of an identity Set are found by equal?, of any other by hash
  # and then equal? or eql?, as CRuby's Set finds them: through @index past
  # eight members, along @data up to eight. In @index a member changed after
  # it joined is looked for under the hash it had then, until reset.
  def find_member(x)
    return @data.any? { |e| e.equal?(x) } if @ident
    idx = @index
    return idx.key?(x) if idx
    h = x.hash
    @data.any? { |e| e.hash == h && (e.equal?(x) || e.eql?(x)) }
  end
  private :find_member

  # The member that leaves @data is the one the lookup found: @index maps
  # each member to itself, and the walk stops on it. Float::NAN, which is not
  # eql? to itself, leaves too.
  def remove_member(x)
    if @ident
      @data.delete_if { |e| e.equal?(x) }
      return
    end
    idx = @index
    if idx
      return unless idx.key?(x)
      m = idx.delete(x)
      i = @data.rindex { |e| e.equal?(m) }
    else
      h = x.hash
      i = @data.rindex { |e| e.hash == h && (e.equal?(x) || e.eql?(x)) }
    end
    @data.delete_at(i) if i
  end
  private :remove_member

  # Up to this many members a Set compares hashes along @data and keeps no
  # @index.
  def walk_size?(n)
    n <= 8
  end
  private :walk_size?

  # A Hash whose keys are the given members, each mapped to itself.
  def index_of(members)
    idx = {}
    members.each { |x| idx[x] = x }
    idx
  end
  private :index_of

  # Members a method has rebuilt whole, in order and unique, become the
  # Set's. The index is built before anything is replaced, so a #hash that
  # raises leaves the Set as it was.
  def adopt(members, ident)
    idx = (ident || walk_size?(members.size)) ? nil : index_of(members)
    @data = members
    @index = idx
    @ident = ident
  end
  private :adopt

  # A new Set that compares as this one does, holding its members: what
  # CRuby's dup gives the combining operators.
  def same_kind_copy
    return Set.new(@data) unless @ident
    r = Set.new
    r.compare_by_identity
    @data.each { |x| r.add(x) }
    r
  end
  private :same_kind_copy

  # A frozen Set refuses every mutation, as CRuby's does: the element array is
  # a private ivar, so freezing the Set itself has to be what the check reads.
  def check_frozen
    raise FrozenError, "can't modify frozen Set: #{inspect}" if frozen?
  end

  # An unfrozen String joins as a frozen copy, as CRuby's Set (and a Hash, for
  # its keys) takes it: changing the String afterwards changes neither the
  # member nor whether the Set holds it, and the member itself is frozen. A
  # frozen String joins as itself. An identity Set takes every String as
  # itself, as CRuby's does: its members are the objects it was handed. add,
  # add? and map! push through here. The test reads the member back from
  # @data, whose elements are boxed: asked of x, where x is a user object's
  # own type, frozen? would mark that class as one whose instances can be
  # frozen, and guard every store to its ivars.
  def push_member(x)
    @data.push(x)
    return if @ident
    e = @data[-1]
    @data[-1] = -e if e.is_a?(String) && !e.frozen?
    idx = @index
    if idx
      k = @data[-1]
      idx[k] = k
    elsif !walk_size?(@data.size)
      @index = index_of(@data)
    end
  end
  private :push_member

  def add(x)
    check_frozen
    push_member(x) unless include?(x)
    self
  end

  def <<(x)
    add(x)
  end

  # add? returns nil when the element was already present.
  def add?(x)
    check_frozen
    return nil if include?(x)
    push_member(x)
    self
  end

  def delete(x)
    check_frozen
    remove_member(x)
    self
  end

  # delete? returns nil when the element was absent.
  def delete?(x)
    check_frozen
    return nil unless include?(x)
    remove_member(x)
    self
  end

  def include?(x)
    find_member(x)
  end
  alias member? include?
  alias === include?

  def each
    # CRuby's Set#each answers an Enumerator when called without a block; the
    # yield below would raise LocalJumpError instead (#3624).
    return @data.dup.each unless block_given?
    _i = 0
    while _i < @data.size
      x = @data[_i]
      yield x
      _i += 1
    end
    self
  end

  def size
    @data.size
  end
  alias length size

  def empty?
    @data.empty?
  end

  def clear
    check_frozen
    @data = []
    @index = nil
    self
  end

  def to_a
    @data.dup
  end

  # CRuby's Set#join: the members' to_a joined. Enumerable has no #join, so a
  # Set read out of a mixed Array fell to Array#join's boxed helper, which
  # rendered it as its inspect text.
  def join(separator = nil)
    to_a.join(separator)
  end

  def map
    r = []
    _i = 0
    while _i < @data.size
      x = @data[_i]
      r.push(yield(x))
      _i += 1
    end
    r
  end
  alias collect map

  # In-place block methods. map!/collect! replace the elements (re-deduplicating);
  # keep_if/delete_if always return self; select!/reject! return self when
  # anything changed and nil otherwise (CRuby's Set contract). map! builds a
  # new plain Set and takes it over, so an identity Set compares by eql?
  # afterwards, as CRuby's does.
  def map!
    check_frozen
    r = []
    _i = 0
    while _i < @data.size
      x = @data[_i]
      r.push(yield(x))
      _i += 1
    end
    t = Set.new
    r.each { |v| t.add(v) }
    @data = []
    @index = nil
    @ident = false
    t.each { |v| push_member(v) }
    self
  end
  alias collect! map!

  def keep_if
    check_frozen
    r = []
    _i = 0
    while _i < @data.size
      x = @data[_i]
      r.push(x) if yield(x)
      _i += 1
    end
    adopt(r, @ident)
    self
  end

  def delete_if
    check_frozen
    r = []
    _i = 0
    while _i < @data.size
      x = @data[_i]
      r.push(x) unless yield(x)
      _i += 1
    end
    adopt(r, @ident)
    self
  end

  def select!
    check_frozen
    n = @data.size
    r = []
    _i = 0
    while _i < @data.size
      x = @data[_i]
      r.push(x) if yield(x)
      _i += 1
    end
    adopt(r, @ident)
    @data.size == n ? nil : self
  end
  alias filter! select!

  def reject!
    check_frozen
    n = @data.size
    r = []
    _i = 0
    while _i < @data.size
      x = @data[_i]
      r.push(x) unless yield(x)
      _i += 1
    end
    adopt(r, @ident)
    @data.size == n ? nil : self
  end

  def merge(enum)
    check_frozen
    Set.check_enum(enum)
    # materialize first: a Range (or any other Enumerable) reaching this poly
    # slot does not dispatch #each through it (#3619)
    enum.to_a.each { |x| add(x) }
    self
  end

  # A Set given to replace is copied, the way it compares included, as
  # CRuby's initialize_copy does.
  def replace(enum)
    check_frozen
    Set.check_enum(enum)
    if Set === enum
      adopt(enum.to_a, enum.compare_by_identity?)
      return self
    end
    @data = []
    @index = nil
    enum.to_a.each { |x| add(x) }
    self
  end

  # reset looks every member up again under the hash it has now, as CRuby's
  # does: members changed after they joined are found again, and two that
  # have become eql? are one. An identity Set's members do not change.
  def reset
    check_frozen
    return self if @ident
    t = []
    idx = {}
    @data.each do |x|
      next if idx.key?(x)
      idx[x] = x
      t.push(x)
    end
    @data = t
    @index = walk_size?(t.size) ? nil : idx
    self
  end

  def subtract(enum)
    check_frozen
    Set.check_enum(enum)
    enum.to_a.each { |x| remove_member(x) }
    self
  end

  # The combining ops reject a non-enumerable operand with ArgumentError rather
  # than letting the #each/#include? call fail as NoMethodError, matching CRuby.
  def self.check_enum(x)
    # a Range reaching this poly slot answers respond_to?(:each) with false,
    # though its members are exactly what every caller here iterates (#3619)
    raise ArgumentError, "value must be enumerable" unless x.respond_to?(:each) || x.is_a?(Range)
    x
  end

  # Set operators build fresh sets; the operand only needs #include? /#each.
  # The intersection ENUMERATES the operand and keeps what the receiver holds
  # -- so a Hash operand contributes its [key, value] pairs, and the result
  # follows the operand's order, as CRuby's does. Asking the operand for
  # #include? instead read a Hash by key (#3676). Between two Sets CRuby
  # walks the smaller and looks each member up in the other, which tells
  # where either compares by identity: the answer is a plain Set.
  def &(other)
    Set.check_enum(other)
    r = Set.new
    if Set === other && size < other.size && (@ident || other.compare_by_identity?)
      @data.each { |x| r.add(x) if other.include?(x) }
      return r
    end
    other.to_a.each { |x| r.add(x) if include?(x) }
    r
  end
  alias intersection &

  # The union and the difference start from a copy, which compares as this
  # Set does.
  def |(other)
    Set.check_enum(other)
    r = same_kind_copy
    other.to_a.each { |x| r.add(x) }
    r
  end
  alias union |
  alias + |

  # The difference enumerates the operand too, for the same reason.
  def -(other)
    Set.check_enum(other)
    r = same_kind_copy
    other.to_a.each { |x| r.delete(x) }
    r
  end
  alias difference -

  # An identity Set's copy takes each member of the operand out if it holds
  # it and in if not, as CRuby's does.
  def ^(other)
    return (self | other) - (self & other) unless @ident
    Set.check_enum(other)
    r = same_kind_copy
    src = other.is_a?(Set) ? other.to_a : Set.new(other).to_a
    src.each { |x| r.include?(x) ? r.delete(x) : r.add(x) }
    r
  end

  # Two Sets that compare their elements differently are not equal unless
  # both are empty.
  def ==(other)
    return true if self.equal?(other)
    return false unless other.is_a?(Set)
    return false unless size == other.size
    return false if size > 0 && @ident != (Set === other && other.compare_by_identity?)
    mark = RecursionGuard.enter_eq(self, other)
    # A pair already being compared answers "equal", which is what lets two
    # distinct self-containing Sets finish. CRuby answers false here, for a
    # reason Spinel's Set does not reproduce: CRuby stores the element's hash
    # while the Set is still empty, and Spinel takes it after the element has
    # joined, so CRuby's membership probe misses where Spinel's would not.
    return true if mark < 0
    r = subset?(other)
    RecursionGuard.leave(mark)
    r
  end
  alias eql? ==

  # Content-based, order-independent: equal sets hash equal (#3069).
  # XOR of the element hashes is commutative, so insertion order is moot.
  def hash
    h = size
    @data.each { |x| h ^= x.hash }
    h
  end

  # The containment predicates take a Set and nothing else: CRuby answers
  # ArgumentError("value must be a set") for anything else, where passing an
  # Array reached `other.subset?` and refused to compile at all.
  def self.check_set(x)
    raise ArgumentError, "value must be a set" unless x.is_a?(Set)
    x
  end

  # A larger Set is no subset, and a smaller no superset, before any member
  # is asked: an identity Set may hold two members a plain one holds as one.
  def subset?(other)
    Set.check_set(other)
    return false if size > other.size
    @data.all? { |x| other.include?(x) }
  end
  alias <= subset?

  def superset?(other)
    Set.check_set(other)
    # Walk the operand rather than calling ITS #subset?: the parameter's type
    # is bound from every call site, so a rejected Array argument still had to
    # answer a Set-only method for the program to compile.
    return false if size < other.size
    other.all? { |x| include?(x) }
  end
  alias >= superset?

  def proper_subset?(other)
    Set.check_set(other)
    size < other.size && subset?(other)
  end
  alias < proper_subset?

  def proper_superset?(other)
    Set.check_set(other)
    size > other.size && other.all? { |x| include?(x) }
  end
  alias > proper_superset?

  # By size first, as CRuby's: two Sets of one size compare 0 or not at all.
  def <=>(other)
    return nil unless other.is_a?(Set)
    if size < other.size
      return -1 if subset?(other)
    elsif size > other.size
      return 1 if superset?(other)
    elsif subset?(other)
      return 0
    end
    nil
  end

  # Where either side compares by identity, CRuby walks the smaller Set and
  # asks the other, and walks any other operand and asks this Set. Only a
  # real Set (Set === other) is asked how it compares: a Set-like object
  # may answer is_a?(Set) with true and have no compare_by_identity?, as
  # ruby/spec's SetSpecs::SetLike does, and CRuby tests the class.
  def disjoint?(other)
    if @ident || (Set === other && other.compare_by_identity?)
      Set.check_enum(other)
      if Set === other && size < other.size
        @data.each { |x| return false if other.include?(x) }
      else
        other.each { |x| return false if include?(x) }
      end
      return true
    end
    @data.each { |x| return false if other.include?(x) }
    true
  end

  def intersect?(other)
    !disjoint?(other)
  end

  # classify { |x| key } -> { key => Set of members }
  def classify
    # The nil pre-write pins k to a boxed (poly) key; see divide.
    h = {}
    @data.each do |x|
      k = nil
      k = yield(x)
      h[k] = Set.new unless h.key?(k)
      h[k].add(x)
    end
    h
  end

  # divide { |x| key } -> Set of member Sets grouped by the block value.
  # divide { |x, y| rel } -> Set of member Sets, each a strongly connected
  # component of the relation graph (mutual reachability, matching CRuby's
  # tsort-based form; a one-way relation does not merge its endpoints).
  def divide(&func)
    if func.arity == 2
      # 2-arity: strongly connected components of the relation graph (mutual
      # reachability, matching CRuby's tsort-based form; a one-way relation
      # does not merge its endpoints). reach is a flat n*n boolean closure.
      a = @data.dup
      n = a.size
      reach = []
      n.times do |i|
        n.times do |j|
          rel = i == j
          rel = true if func.call(a[i], a[j])
          reach.push(rel)
        end
      end
      n.times do |k|
        n.times do |i|
          n.times do |j|
            reach[i * n + j] = true if reach[i * n + k] && reach[k * n + j]
          end
        end
      end
      rep = []
      n.times do |i|
        ri = i
        j = 0
        while j < i
          if reach[i * n + j] && reach[j * n + i]
            ri = rep[j]
            break
          end
          j += 1
        end
        rep.push(ri)
      end
      groups = {}
      n.times do |i|
        k = rep[i]
        groups[k] = Set.new unless groups.key?(k)
        groups[k].add(a[i])
      end
      res = Set.new
      groups.each_value { |s| res.add(s) }
      res
    else
      # 1-arity: group by the block value. The nil pre-write pins k to a
      # boxed (poly) key: call sites with different block value types share
      # this body, and a concretely-typed k would collapse them to one type.
      h = {}
      @data.each do |x|
        k = nil
        k = func.call(x)
        h[k] = Set.new unless h.key?(k)
        h[k].add(x)
      end
      r = Set.new
      h.each_value { |s| r.add(s) }
      r
    end
  end

  # flatten recursively merges nested Set elements into a flat Set. A Set that
  # holds itself has no flat form, so CRuby raises rather than looping.
  def flatten
    mark = RecursionGuard.enter_flatten(self)
    raise ArgumentError, "tried to flatten recursive Set" if mark < 0
    r = Set.new
    @data.each do |x|
      if x.is_a?(Set)
        x.flatten.each { |y| r.add(y) }
      else
        r.add(x)
      end
    end
    RecursionGuard.leave(mark)
    r
  end

  # flatten!: in-place flatten; returns self when any nested Set was merged,
  # nil when the set was already flat (CRuby's contract). Rebuilds @data
  # through add rather than assigning flatten.to_a into it -- that write
  # would make @data's inferred type depend on itself (to_a dups @data)
  # and widen the ivar to poly.
  def flatten!
    check_frozen
    nested = false
    @data.each { |x| nested = true if x.is_a?(Set) }
    return nil unless nested
    f = flatten
    @data = []
    @index = nil
    # CRuby replaces the Set with the flat one, which compares by eql?
    @ident = false
    f.each { |y| add(y) }
    self
  end

  def inspect
    mark = RecursionGuard.enter_inspect(self)
    return "Set[...]" if mark < 0
    r = "Set[" + @data.map { |x| x.inspect }.join(", ") + "]"
    RecursionGuard.leave(mark)
    r
  end
  alias to_s inspect
end
