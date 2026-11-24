# A class-body macro call whose run takes a branch that defines nothing (an
# early `return`) is still the macro's form: left as written, the call would
# meet the define_method of a computed name in its body, which is refused,
# where CRuby runs the same branch and defines nothing.
module Macros
  def attribute(name)
    return if name == :id
    define_method(name) { name.to_s }
  end

  # the branch that defines nothing writes class state first
  def column(name, type)
    if name.to_s == "id"
      self.id_type = type
      return
    end
    define_method(name) { type }
  end

  # a nested macro call that defines nothing, beside one that does
  def pair(a, b)
    attribute a
    attribute b
  end
end

class Todo
  extend Macros
  attribute :title
  attribute :id
end
p Todo.new.title
p Todo.new.respond_to?(:id)

class Row
  extend Macros
  def self.id_type=(t)
    @id_type = t
  end
  def self.id_type = @id_type
  column :name, :string
  column :id, :integer
  column :done, :boolean
end
p Row.new.name, Row.new.done
p Row.id_type
p Row.new.respond_to?(:id)

class Note
  extend Macros
  pair :id, :body
  pair :id, :id
end
p Note.new.body
p Note.new.respond_to?(:id)
