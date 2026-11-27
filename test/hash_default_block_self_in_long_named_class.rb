# A Hash with a default block made in an instance method (`@cache = Hash.new
# { |h, pid| h[pid] = {} }`) runs the block with the method's self, which the
# block's C function casts from the pointer it is handed. The cast named the
# struct by the class's Ruby name, where the struct is declared by its C name:
# the two differ for a class whose qualified name is long enough to be hashed
# (a large application program's connection adapter's statement pool), and
# clang stopped at the undeclared type.
module ActiveRecord
  module ConnectionAdapters
    class StatementPoolWithAVeryLongClassNameThatGetsHashed
      def initialize(statement_limit = nil)
        @cache = Hash.new { |h, pid| h[pid] = {} }
        @statement_limit = statement_limit || 1000
      end
      def [](key) = @cache[key]
      def limit = @statement_limit
    end
    class SQLite3AdapterWithAVeryLongClassNameThatGetsHashedToo
      def self.type_cast_config_to_integer(v) = v.nil? ? nil : Integer(v)
      def initialize(config) = @config = config
      def build_statement_pool = StatementPoolWithAVeryLongClassNameThatGetsHashed.new(self.class.type_cast_config_to_integer(@config[:statement_limit]))
      def statements = @statements ||= build_statement_pool
    end
  end
end
a = ActiveRecord::ConnectionAdapters::SQLite3AdapterWithAVeryLongClassNameThatGetsHashedToo.new({ statement_limit: "5" })
b = ActiveRecord::ConnectionAdapters::SQLite3AdapterWithAVeryLongClassNameThatGetsHashedToo.new({})
p a.statements.limit, b.statements.limit
pool = a.statements
pool[1][:a] = "x"
p pool[1], pool[2], pool[1].equal?(pool[1])
