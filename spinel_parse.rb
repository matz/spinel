#!/usr/bin/env ruby
# Spinel Parse - AST Serializer
#
# Parses Ruby source with Prism and outputs a binary AST that
# spinel_codegen.rb can consume. This is the only part that
# depends on the Prism C extension gem.
#
# Usage: ruby spinel_parse.rb input.rb > ast.bin
#   or:  ruby spinel_parse.rb input.rb --json > ast.json  (legacy JSON mode)

require "prism"
require "json"

# Recursively resolve require_relative and inline file contents,
# exactly matching the original spinel.rb behavior.
def resolve_requires(source, source_path)
  base_dir = File.dirname(File.expand_path(source_path))
  resolved = source.dup
  resolved.gsub!(/^require_relative\s+["'](.+?)["']\s*$/) do
    rel_path = $1
    req_file = File.join(base_dir, rel_path)
    req_file += ".rb" unless req_file.end_with?(".rb")
    if File.exist?(req_file)
      content = File.read(req_file)
      resolve_requires(content, req_file)
    else
      "# require_relative not found: #{rel_path}"
    end
  end
  resolved
end

def serialize_node(node)
  return nil if node.nil?

  case node
  when Prism::ProgramNode
    {
      "type" => "ProgramNode",
      "statements" => serialize_node(node.statements)
    }

  when Prism::StatementsNode
    {
      "type" => "StatementsNode",
      "body" => node.body.map { |s| serialize_node(s) }
    }

  when Prism::ClassNode
    {
      "type" => "ClassNode",
      "constant_path" => serialize_node(node.constant_path),
      "superclass" => serialize_node(node.superclass),
      "body" => serialize_node(node.body)
    }

  when Prism::ModuleNode
    {
      "type" => "ModuleNode",
      "constant_path" => serialize_node(node.constant_path),
      "body" => serialize_node(node.body)
    }

  when Prism::DefNode
    {
      "type" => "DefNode",
      "name" => node.name.to_s,
      "parameters" => serialize_parameters(node.parameters),
      "body" => serialize_node(node.body),
      "receiver" => serialize_node(node.receiver)
    }

  when Prism::CallNode
    result = {
      "type" => "CallNode",
      "name" => node.name.to_s,
      "receiver" => serialize_node(node.receiver),
      "arguments" => serialize_arguments(node.arguments),
      "block" => serialize_node(node.block)
    }
    # Serialize safe navigation operator (&.)
    if node.call_operator == "&."
      result["call_operator"] = "&."
    end
    result

  when Prism::ConstantWriteNode
    {
      "type" => "ConstantWriteNode",
      "name" => node.name.to_s,
      "value" => serialize_node(node.value)
    }

  when Prism::ConstantPathWriteNode
    {
      "type" => "ConstantPathWriteNode",
      "value" => serialize_node(node.value),
      "target" => serialize_node(node.target)
    }

  when Prism::ConstantReadNode
    {
      "type" => "ConstantReadNode",
      "name" => node.name.to_s
    }

  when Prism::ConstantPathNode
    {
      "type" => "ConstantPathNode",
      "parent" => serialize_node(node.parent),
      "name" => node.name.to_s
    }

  when Prism::LocalVariableWriteNode
    {
      "type" => "LocalVariableWriteNode",
      "name" => node.name.to_s,
      "value" => serialize_node(node.value)
    }

  when Prism::LocalVariableReadNode
    {
      "type" => "LocalVariableReadNode",
      "name" => node.name.to_s
    }

  when Prism::LocalVariableOperatorWriteNode
    {
      "type" => "LocalVariableOperatorWriteNode",
      "name" => node.name.to_s,
      "binary_operator" => node.binary_operator.to_s,
      "value" => serialize_node(node.value)
    }

  when Prism::LocalVariableTargetNode
    {
      "type" => "LocalVariableTargetNode",
      "name" => node.name.to_s
    }

  when Prism::InstanceVariableWriteNode
    {
      "type" => "InstanceVariableWriteNode",
      "name" => node.name.to_s,
      "value" => serialize_node(node.value)
    }

  when Prism::InstanceVariableReadNode
    {
      "type" => "InstanceVariableReadNode",
      "name" => node.name.to_s
    }

  when Prism::InstanceVariableTargetNode
    {
      "type" => "InstanceVariableTargetNode",
      "name" => node.name.to_s
    }

  when Prism::InstanceVariableAndWriteNode
    {
      "type" => "InstanceVariableAndWriteNode",
      "name" => node.name.to_s,
      "value" => serialize_node(node.value)
    }

  when Prism::InstanceVariableOrWriteNode
    {
      "type" => "InstanceVariableOrWriteNode",
      "name" => node.name.to_s,
      "value" => serialize_node(node.value)
    }

  when Prism::InstanceVariableOperatorWriteNode
    {
      "type" => "InstanceVariableOperatorWriteNode",
      "name" => node.name.to_s,
      "binary_operator" => node.binary_operator.to_s,
      "value" => serialize_node(node.value)
    }

  when Prism::GlobalVariableWriteNode
    {
      "type" => "GlobalVariableWriteNode",
      "name" => node.name.to_s,
      "value" => serialize_node(node.value)
    }

  when Prism::GlobalVariableReadNode
    {
      "type" => "GlobalVariableReadNode",
      "name" => node.name.to_s
    }

  when Prism::IntegerNode
    {
      "type" => "IntegerNode",
      "value" => node.value
    }

  when Prism::FloatNode
    {
      "type" => "FloatNode",
      "value" => node.value
    }

  when Prism::StringNode
    {
      "type" => "StringNode",
      "content" => node.content
    }

  when Prism::InterpolatedStringNode
    {
      "type" => "InterpolatedStringNode",
      "parts" => node.parts.map { |p| serialize_node(p) }
    }

  when Prism::EmbeddedStatementsNode
    {
      "type" => "EmbeddedStatementsNode",
      "statements" => serialize_node(node.statements)
    }

  when Prism::SymbolNode
    {
      "type" => "SymbolNode",
      "value" => node.value
    }

  when Prism::TrueNode
    { "type" => "TrueNode" }

  when Prism::FalseNode
    { "type" => "FalseNode" }

  when Prism::NilNode
    { "type" => "NilNode" }

  when Prism::SelfNode
    { "type" => "SelfNode" }

  when Prism::ArrayNode
    {
      "type" => "ArrayNode",
      "elements" => node.elements.map { |e| serialize_node(e) }
    }

  when Prism::HashNode
    {
      "type" => "HashNode",
      "elements" => node.elements.map { |e| serialize_node(e) }
    }

  when Prism::AssocNode
    {
      "type" => "AssocNode",
      "key" => serialize_node(node.key),
      "value" => serialize_node(node.value)
    }

  when Prism::KeywordHashNode
    {
      "type" => "KeywordHashNode",
      "elements" => node.elements.map { |e| serialize_node(e) }
    }

  when Prism::RangeNode
    {
      "type" => "RangeNode",
      "left" => serialize_node(node.left),
      "right" => serialize_node(node.right)
    }

  when Prism::IfNode
    {
      "type" => "IfNode",
      "predicate" => serialize_node(node.predicate),
      "statements" => serialize_node(node.statements),
      "subsequent" => serialize_node(node.subsequent)
    }

  when Prism::ElseNode
    {
      "type" => "ElseNode",
      "statements" => serialize_node(node.statements)
    }

  when Prism::UnlessNode
    {
      "type" => "UnlessNode",
      "predicate" => serialize_node(node.predicate),
      "statements" => serialize_node(node.statements),
      "else_clause" => serialize_node(node.else_clause)
    }

  when Prism::WhileNode
    {
      "type" => "WhileNode",
      "predicate" => serialize_node(node.predicate),
      "statements" => serialize_node(node.statements)
    }

  when Prism::UntilNode
    {
      "type" => "UntilNode",
      "predicate" => serialize_node(node.predicate),
      "statements" => serialize_node(node.statements)
    }

  when Prism::ForNode
    {
      "type" => "ForNode",
      "index" => serialize_node(node.index),
      "collection" => serialize_node(node.collection),
      "statements" => serialize_node(node.statements)
    }

  when Prism::CaseNode
    {
      "type" => "CaseNode",
      "predicate" => serialize_node(node.predicate),
      "conditions" => (node.conditions || []).map { |c| serialize_node(c) },
      "else_clause" => serialize_node(node.else_clause)
    }

  when Prism::CaseMatchNode
    {
      "type" => "CaseMatchNode",
      "predicate" => serialize_node(node.predicate),
      "conditions" => (node.conditions || []).map { |c| serialize_node(c) },
      "else_clause" => serialize_node(node.else_clause)
    }

  when Prism::WhenNode
    {
      "type" => "WhenNode",
      "conditions" => node.conditions.map { |c| serialize_node(c) },
      "statements" => serialize_node(node.statements)
    }

  when Prism::InNode
    {
      "type" => "InNode",
      "pattern" => serialize_node(node.pattern),
      "statements" => serialize_node(node.statements)
    }

  when Prism::BeginNode
    {
      "type" => "BeginNode",
      "statements" => serialize_node(node.statements),
      "rescue_clause" => serialize_node(node.rescue_clause),
      "ensure_clause" => serialize_node(node.ensure_clause),
      "else_clause" => serialize_node(node.else_clause)
    }

  when Prism::EnsureNode
    {
      "type" => "EnsureNode",
      "statements" => serialize_node(node.statements)
    }

  when Prism::RescueNode
    {
      "type" => "RescueNode",
      "exceptions" => (node.exceptions || []).map { |e| serialize_node(e) },
      "reference" => serialize_node(node.reference),
      "statements" => serialize_node(node.statements),
      "subsequent" => serialize_node(node.subsequent)
    }

  when Prism::RescueModifierNode
    {
      "type" => "RescueModifierNode",
      "expression" => serialize_node(node.expression),
      "rescue_expression" => serialize_node(node.rescue_expression)
    }

  when Prism::ReturnNode
    {
      "type" => "ReturnNode",
      "arguments" => serialize_arguments(node.arguments)
    }

  when Prism::BreakNode
    { "type" => "BreakNode" }

  when Prism::NextNode
    { "type" => "NextNode" }

  when Prism::RetryNode
    { "type" => "RetryNode" }

  when Prism::YieldNode
    {
      "type" => "YieldNode",
      "arguments" => serialize_arguments(node.arguments)
    }

  when Prism::BlockNode
    {
      "type" => "BlockNode",
      "parameters" => serialize_block_parameters(node.parameters),
      "body" => serialize_node(node.body)
    }

  when Prism::RequiredParameterNode
    {
      "type" => "RequiredParameterNode",
      "name" => node.name.to_s
    }

  when Prism::RestParameterNode
    {
      "type" => "RestParameterNode",
      "name" => node.name&.to_s
    }

  when Prism::BlockParameterNode
    {
      "type" => "BlockParameterNode",
      "name" => node.name&.to_s
    }

  when Prism::BlockLocalVariableNode
    {
      "type" => "BlockLocalVariableNode",
      "name" => node.name.to_s
    }

  when Prism::ParenthesesNode
    {
      "type" => "ParenthesesNode",
      "body" => serialize_node(node.body)
    }

  when Prism::AndNode
    {
      "type" => "AndNode",
      "left" => serialize_node(node.left),
      "right" => serialize_node(node.right)
    }

  when Prism::OrNode
    {
      "type" => "OrNode",
      "left" => serialize_node(node.left),
      "right" => serialize_node(node.right)
    }

  when Prism::DefinedNode
    {
      "type" => "DefinedNode",
      "value" => serialize_node(node.value)
    }

  when Prism::SourceLineNode
    {
      "type" => "SourceLineNode",
      "start_line" => node.location.start_line
    }

  when Prism::SplatNode
    {
      "type" => "SplatNode",
      "expression" => serialize_node(node.expression)
    }

  when Prism::SuperNode
    {
      "type" => "SuperNode",
      "arguments" => serialize_arguments(node.arguments)
    }

  when Prism::ForwardingSuperNode
    { "type" => "ForwardingSuperNode" }

  when Prism::MultiWriteNode
    {
      "type" => "MultiWriteNode",
      "lefts" => node.lefts.map { |l| serialize_node(l) },
      "value" => serialize_node(node.value)
    }

  when Prism::LambdaNode
    { "type" => "LambdaNode" }

  when Prism::XStringNode
    {
      "type" => "XStringNode",
      "content" => node.content
    }

  when Prism::RegularExpressionNode
    {
      "type" => "RegularExpressionNode",
      "unescaped" => node.unescaped
    }

  when Prism::NumberedReferenceReadNode
    {
      "type" => "NumberedReferenceReadNode",
      "number" => node.number
    }

  when Prism::MatchWriteNode
    {
      "type" => "MatchWriteNode",
      "call" => serialize_node(node.call)
    }

  when Prism::AlternationPatternNode
    {
      "type" => "AlternationPatternNode",
      "left" => serialize_node(node.left),
      "right" => serialize_node(node.right)
    }

  when Prism::NumberedParametersNode
    {
      "type" => "NumberedParametersNode",
      "maximum" => node.maximum
    }

  else
    # Fallback for any unhandled node types
    { "type" => node.class.name.sub("Prism::", "") }
  end
end

# Serialize ParametersNode (for DefNode.parameters)
def serialize_parameters(node)
  return nil if node.nil?

  case node
  when Prism::ParametersNode
    result = {
      "type" => "ParametersNode",
      "requireds" => (node.requireds || []).map { |p| serialize_node(p) },
      "optionals" => (node.optionals || []).map { |p| serialize_optional_param(p) },
      "keywords" => (node.keywords || []).map { |kw| serialize_keyword_param(kw) }
    }
    result["rest"] = serialize_node(node.rest) if node.rest
    result["block"] = serialize_node(node.block) if node.block
    result
  else
    serialize_node(node)
  end
end

# Serialize optional parameter (has name and value)
def serialize_optional_param(node)
  return nil if node.nil?
  {
    "type" => node.class.name.sub("Prism::", ""),
    "name" => node.name.to_s,
    "value" => serialize_node(node.value)
  }
end

# Serialize keyword parameter
def serialize_keyword_param(node)
  return nil if node.nil?
  result = {
    "type" => node.class.name.sub("Prism::", ""),
    "name" => node.name.to_s
  }
  result["value"] = serialize_node(node.value) if node.respond_to?(:value) && node.value
  result
end

# Serialize block parameters (for BlockNode.parameters)
def serialize_block_parameters(node)
  return nil if node.nil?

  case node
  when Prism::BlockParametersNode
    {
      "type" => "BlockParametersNode",
      "parameters" => serialize_parameters(node.parameters)
    }
  when Prism::NumberedParametersNode
    {
      "type" => "NumberedParametersNode",
      "maximum" => node.maximum
    }
  else
    serialize_node(node)
  end
end

# Serialize ArgumentsNode (for CallNode.arguments, ReturnNode.arguments, etc.)
def serialize_arguments(node)
  return nil if node.nil?

  case node
  when Prism::ArgumentsNode
    {
      "type" => "ArgumentsNode",
      "arguments" => (node.arguments || []).map { |a| serialize_node(a) }
    }
  else
    serialize_node(node)
  end
end

# --- Binary AST serialization ---
# Format:
#   TAG_NIL    = 0  (no payload)
#   TAG_INT    = 1  (8 bytes LE signed)
#   TAG_STRING = 2  (4 bytes LE length + bytes)
#   TAG_NODE   = 3  (type_string + field_count(2 bytes LE) + fields)
#   TAG_ARRAY  = 4  (4 bytes LE count + elements)
#   TAG_FLOAT  = 5  (8 bytes LE double)
#   TAG_BOOL   = 6  (1 byte: 0=false, 1=true)
#
# Each node field: key_string + value
# key_string: 2 bytes LE length + bytes (short strings)

TAG_NIL    = 0
TAG_INT    = 1
TAG_STRING = 2
TAG_NODE   = 3
TAG_ARRAY  = 4
TAG_FLOAT  = 5
TAG_BOOL   = 6

def write_bin_nil(out)
  out << [TAG_NIL].pack("C")
end

def write_bin_int(out, val)
  out << [TAG_INT].pack("C")
  out << [val].pack("q<")
end

def write_bin_string(out, str)
  out << [TAG_STRING].pack("C")
  bytes = str.encode("UTF-8")
  out << [bytes.bytesize].pack("V")
  out << bytes
end

def write_bin_float(out, val)
  out << [TAG_FLOAT].pack("C")
  out << [val].pack("E")
end

def write_bin_bool(out, val)
  out << [TAG_BOOL].pack("C")
  out << [val ? 1 : 0].pack("C")
end

def write_bin_short_string(out, str)
  bytes = str.encode("UTF-8")
  out << [bytes.bytesize].pack("v")
  out << bytes
end

def write_bin_hash(out, hash)
  return write_bin_nil(out) if hash.nil?

  # Determine the node type
  type_str = hash["type"]
  unless type_str
    write_bin_nil(out)
    return
  end

  out << [TAG_NODE].pack("C")
  write_bin_short_string(out, type_str)

  # Collect non-type fields
  fields = hash.reject { |k, _| k == "type" }
  out << [fields.size].pack("v")

  fields.each do |key, value|
    write_bin_short_string(out, key)
    write_bin_value(out, value)
  end
end

def write_bin_value(out, value)
  case value
  when nil
    write_bin_nil(out)
  when Integer
    write_bin_int(out, value)
  when Float
    write_bin_float(out, value)
  when String
    write_bin_string(out, value)
  when true
    write_bin_bool(out, true)
  when false
    write_bin_bool(out, false)
  when Hash
    if value["type"]
      write_bin_hash(out, value)
    else
      # Non-node hash - serialize as nil (shouldn't happen in AST)
      write_bin_nil(out)
    end
  when Array
    out << [TAG_ARRAY].pack("C")
    out << [value.size].pack("V")
    value.each { |elem| write_bin_value(out, elem) }
  else
    write_bin_nil(out)
  end
end

# --- Main ---
source_file = ARGV[0]
json_mode = ARGV.include?("--json")
unless source_file
  $stderr.puts "Usage: ruby spinel_parse.rb input.rb > ast.bin"
  $stderr.puts "       ruby spinel_parse.rb input.rb --json > ast.json"
  exit 1
end

source = File.read(source_file)
source = resolve_requires(source, source_file)
result = Prism.parse(source)
ast = serialize_node(result.value)

if json_mode
  puts JSON.generate(ast)
else
  out = String.new("", encoding: "BINARY")
  write_bin_hash(out, ast)
  $stdout.binmode
  $stdout.write(out)
end
