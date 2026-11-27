# A yielding method is spliced at each call. Its body here binds the named
# captures of a regex match (`/(?<table>..)\.(?<column>..)/ =~ field`) and
# reads them. Two things went wrong in the splice. The match-write looked the
# capture local up by its per-inline C name, found nothing, and wrote the
# bytes into a local declared as a String handle. And the String conversion
# of the match's subject, a boxed local of the body (`field = field.to_s`
# where a Symbol or a String comes in), was held in front of the enclosing
# call, as an argument's conversion is, where the local was not declared yet.
# A large application program's column resolver (`arel_column`) has both.
class Table
  def initialize(name) = @name = name
  def [](f) = "#{@name}.#{f}"
end
class Q
  ALIASES = { "nick" => "name" }
  COLUMNS = { "name" => true, "age" => true }
  def initialize = @table = Table.new("users")
  def table = @table
  def arel_column_with_table(table, column, references: true) = "#{table}.#{column}#{references ? "!" : ""}"
  def arel_column(field, references: true)
    field = field.name if is_symbol = field.is_a?(Symbol)
    field = ALIASES[field] || field
    if COLUMNS.key?(field)
      table[field]
    elsif /\A(?<table>(?:\w+\.)?\w+)\.(?<column>\w+)\z/ =~ field
      arel_column_with_table(table, column, references: references)
    elsif block_given?
      yield field
    else
      "#{field}?#{is_symbol}"
    end
  end
end
q = Q.new
p q.arel_column(:nick), q.arel_column("age"), q.arel_column("posts.title"), q.arel_column("x.y.z", references: false)
p q.arel_column("other") { |f| "blk #{f}" }, q.arel_column("other")

def arel_column(field)
  field = field.to_s
  if /\A(?<table>\w+)\.(?<column>\w+)\z/ =~ field
    "#{table}.#{column}"
  elsif block_given?
    yield field
  else
    "#{field}?"
  end
end
p arel_column(:nick), arel_column("posts.title"), arel_column("other"), arel_column("other") { |f| "blk #{f}" }
