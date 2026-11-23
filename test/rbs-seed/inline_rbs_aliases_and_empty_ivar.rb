# spinel: rbs-seed-run
# Inline `# @rbs type name = T` aliases and statementless class-body `# @rbs @ivar: T`
# declarations working together under rbs-seed-test and share-strings-test (GC stress).

# @rbs type str_map = Hash[String, String]
# @rbs type maybe_count = count_t?
# @rbs type count_t = Integer

INLINE_ALIAS_CONF = {
  "a" => { "k" => "v" },
  "b" => 2
}.freeze

class AliasSeedBase
  def initialize
    @count = nil
    @name = nil
  end

  def count = @count
  def name = @name
  def set_count(n) = (@count = n)
  def set_name(s) = (@name = s)
end

class AliasSeedSub < AliasSeedBase
  # @rbs type name_t = String?
  # @rbs @count: maybe_count
  # @rbs @name: name_t
end

class AliasSeedCfg
  #: (String) -> str_map
  def fetch_map(key)
    INLINE_ALIAS_CONF[key]
  end
end

sub = AliasSeedSub.new
p sub.count, sub.name
sub.set_count(7)
sub.set_name("hello")
p sub.count, sub.name

cfg = AliasSeedCfg.new
p cfg.fetch_map("a")["k"]
