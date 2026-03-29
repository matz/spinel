# SpNode: AST node representation for Spinel self-hosting
#
# All attributes are explicitly declared (no method_missing).
# Spinel compiles this as a struct with fields.

class SpNode
  attr_accessor :type       # String: "IntegerNode", "CallNode", etc.
  attr_accessor :name       # String: method/variable name
  attr_accessor :value      # Integer/Float/String: literal value
  attr_accessor :content    # String: string content
  attr_accessor :receiver   # SpNode: call receiver
  attr_accessor :arguments  # Array of SpNode: call arguments
  attr_accessor :body       # SpNode: method/class/block body
  attr_accessor :statements # SpNode: statements node
  attr_accessor :block      # SpNode: block node
  attr_accessor :parameters # SpNode: parameters node
  attr_accessor :predicate  # SpNode: if/while condition
  attr_accessor :conditions # Array of SpNode: case/when conditions
  attr_accessor :subsequent # SpNode: elsif/else chain
  attr_accessor :left       # SpNode: binary left
  attr_accessor :right      # SpNode: binary right
  attr_accessor :elements   # Array of SpNode: array/hash elements
  attr_accessor :parts      # Array of SpNode: interpolated string parts
  attr_accessor :constant_path # SpNode: class name constant
  attr_accessor :superclass # SpNode: parent class
  attr_accessor :else_clause # SpNode: else branch
  attr_accessor :rescue_clause # SpNode: rescue branch
  attr_accessor :ensure_clause # SpNode: ensure branch
  attr_accessor :expression # SpNode: rescue modifier expression
  attr_accessor :requireds  # Array of SpNode: required params
  attr_accessor :optionals  # Array of SpNode: optional params
  attr_accessor :rest       # SpNode: rest param
  attr_accessor :keywords   # Array of SpNode: keyword params
  attr_accessor :key        # SpNode: hash key
  attr_accessor :reference  # SpNode: numbered reference ($1)
  attr_accessor :exceptions # Array of SpNode: rescue exception classes
  attr_accessor :target     # SpNode: multi-write target
  attr_accessor :targets    # Array of SpNode: multi-write targets
  attr_accessor :depth      # Integer: variable depth
  attr_accessor :operator   # String: operator for op-write
  attr_accessor :binary_operator # String: binary operator
  attr_accessor :pattern    # SpNode: pattern match pattern
  attr_accessor :parent     # SpNode: parent (for traversal)
  attr_accessor :flags      # Integer: node flags
  attr_accessor :index      # SpNode: index expression
  attr_accessor :unescaped  # String: raw string value

  def initialize
    @type = ""
    @name = ""
    @value = 0
    @content = ""
    @depth = 0
    @flags = 0
    @operator = ""
    @binary_operator = ""
    @unescaped = ""
  end
end
