# Spinel bundled `rexml/document`: REXML's tree API, written in Ruby.
#
# CRuby's REXML is a pure-Ruby gem whose parser and node classes lean on
# runtime features a compiled program does not have (`instance_variable_get`,
# a Hash subclass holding Attributes or Hashes, `__send__`). This package
# ports the part a program uses to read an XML document, change it and write
# it back, following REXML 3.4's code path by path:
#
# - the parser is REXML's BaseParser and TreeParser (without DOCTYPE),
#   including its input source, which reads the document in `>`-terminated
#   chunks: a ParseException carries REXML's message, line, position and
#   "Last 80 unconsumed characters" for the same input;
# - Document, Element, Elements, Attributes, Attribute, Text, CData, Comment,
#   Instruction and XMLDecl answer as REXML's do (parsed text and attribute
#   values keep their source spelling and `value` / `text` expand the
#   references; a value a program sets is escaped when written; a childless
#   element is written `<x/>`; attributes are written in single quotes,
#   sorted by local name);
# - a DocType a program builds (`DocType.new("html")`, a name with its
#   external ID in one String, or `[name, id_type, public_id, system_id]`)
#   is placed and written as REXML places and writes it;
# - Formatters::Default and Formatters::Pretty (`compact`, `width`) write
#   every node as REXML's do: `to_s` uses Default, and Document#write and
#   Element#write with an indent use Pretty, which squeezes and wraps texts
#   (and, as REXML's does, squeezes the written Text node itself).
#
# Not here, raised as NotImplementedError where a program reaches them: a
# DOCTYPE in a parsed source (and so entity, element, attribute-list and
# notation declarations), XPath beyond a child path of names and `*`
# (predicates, `//`, `..`, axes, functions, an empty path), an encoding other
# than UTF-8, a Document context Hash, a source that is not a String, and
# #write with `transitive` (the Transitive formatter). Document#write takes
# its arguments in order, not as REXML's options Hash. The pull, stream and
# SAX2 parsers and REXML::XPath are absent.
#
# Known differences: a ParseException REXML raises around another exception
# (an illegal character in a text or attribute, a second root element) quotes
# that exception's backtrace in its message; a Spinel release build has no
# backtrace, so those lines are empty. Attributes that share a local name
# (`a` and `p:a`) are written in source order; REXML sorts with `sort_by`,
# which does not promise an order for equal keys. Element#write does not
# print REXML's deprecation warning.

module REXML
  NCNAME_START = "A-Z_a-z\\u00C0-\\u00D6\\u00D8-\\u00F6\\u00F8-\\u02FF\\u0370-\\u037D\\u037F-\\u1FFF\\u200C-\\u200D\\u2070-\\u218F\\u2C00-\\u2FEF\\u3001-\\uD7FF\\uF900-\\uFDCF\\uFDF0-\\uFFFD\\u{10000}-\\u{EFFFF}"
  NCNAME_CHARS = NCNAME_START + "\\-\\.0-9\\u00B7\\u0300-\\u036F\\u203F-\\u2040"
  # XMLTokens: the names of nodes and entity references.
  NCNAME_STR = "[#{NCNAME_START}][#{NCNAME_CHARS}]*"
  NAME_STR = "[:#{NCNAME_START}][:#{NCNAME_CHARS}]*"
  NAME_WITHOUT_NAMESPACE = /\A#{NCNAME_STR}\z/
  NAMESPLIT = /^(?:(#{NCNAME_STR}):)?(#{NCNAME_STR})/u
  REFERENCE_RE = /(?:&#{NAME_STR};|&#\d+;|&#x[0-9a-fA-F]+;)/
  XPATH_STEP = /\A(?:\*|(?:#{NCNAME_STR}:)?#{NCNAME_STR})\z/
  ENTITY_EXPANSION_TEXT_LIMIT = 10240

  class ParseException < RuntimeError
    attr_reader :line, :position

    # `source` is the parser's input; REXML's message then adds where the
    # parser stood. `exception`: the error this one wraps.
    def initialize(message, source = nil, exception = nil)
      err = +""
      unless exception.nil?
        err << exception.inspect << "\n" << exception.backtrace.to_a.join("\n") << "\n...\n"
      end
      err << message
      @line = nil
      @position = nil
      unless source.nil?
        @line = source.error_line
        @position = source.error_position
        err << "\nLine: #{@line}\nPosition: #{@position}\nLast 80 unconsumed characters:\n"
        err << source.buffer[0, 81].gsub("\n", " ")
      end
      super(err)
    end
  end

  class UndefinedNamespaceException < ParseException
    def initialize(prefix)
      super("Undefined prefix #{prefix} found")
    end
  end

  # How REXML's Text writes and reads references.
  module Entities
    def self.normalize(input)
      copy = input.to_s
      copy = copy.gsub("&", "&amp;") if copy.include?("&")
      copy = copy.gsub(">", "&gt;") if copy.include?(">")
      copy = copy.gsub("<", "&lt;") if copy.include?("<")
      copy = copy.gsub("\"", "&quot;") if copy.include?("\"")
      copy = copy.gsub("'", "&apos;") if copy.include?("'")
      copy
    end

    def self.unnormalize(string)
      sum = 0
      string.gsub(/\r\n?/, "\n").gsub(REFERENCE_RE) do |ref|
        s = expand(ref)
        raise "entity expansion has grown too large" if sum + s.bytesize > ENTITY_EXPANSION_TEXT_LIMIT
        sum += s.bytesize
        s
      end
    end

    def self.expand(ref)
      if ref[1] == "#"
        if ref[2] == "x"
          [ref[3, ref.length - 4].to_i(16)].pack("U*")
        else
          [ref[2, ref.length - 3].to_i].pack("U*")
        end
      elsif ref == "&amp;" then "&"
      elsif ref == "&gt;" then ">"
      elsif ref == "&lt;" then "<"
      elsif ref == "&quot;" then "\""
      elsif ref == "&apos;" then "'"
      else ref
      end
    end

    def self.valid_char?(c)
      c == 0x9 || c == 0xA || c == 0xD || (c >= 0x20 && c <= 0xD7FF) ||
        (c >= 0xE000 && c <= 0xFFFD) || (c >= 0x10000 && c <= 0x10FFFF)
    end

    # Text.check: what a raw (parsed) string may hold.
    def self.check(string)
      string.each_char do |ch|
        raise "Illegal character #{ch.inspect} in raw string #{string.inspect}" unless valid_char?(ch.ord)
      end
      pos = 0
      while (index = string.index(/<|&/, pos))
        if string[index] == "<"
          raise "Illegal character \"<\" in raw string #{string.inspect}"
        end
        end_index = string.index(/[^\s];/, index + 1)
        raise "Illegal character \"&\" in raw string #{string.inspect}" if end_index.nil?
        value = string[index + 1, end_index - index]
        raise "Illegal character \"&\" in raw string #{string.inspect}" if value.match?(/\s/)
        if value[0] == "#"
          ref = value[1, value.length - 1]
          unless ref.match?(/\A(\d+|x[0-9a-fA-F]+)\z/)
            if ref[0] == "x" || ref[ref.length - 1] == "x"
              raise "Illegal character \"&\" in raw string #{string.inspect}"
            else
              raise "Illegal character #{string.inspect} in raw string #{string.inspect}"
            end
          end
          code = ref[0] == "x" ? ref[1, ref.length - 1].to_i(16) : ref.to_i
          raise "Illegal character #{string.inspect} in raw string #{string.inspect}" unless valid_char?(code)
        elsif !value.match?(/\A#{NAME_STR}\z/)
          raise "Illegal character \"&\" in raw string #{string.inspect}"
        end
        pos = end_index + 1
      end
      string
    end
  end

  # A node with a parent: everything but a Document.
  class Child
    attr_reader :parent

    def initialize
      @parent = nil
    end

    def parent=(other)
      return @parent if @parent.equal?(other)
      @parent.delete(self) unless @parent.nil?
      @parent = other
    end

    def remove
      @parent.delete(self) unless @parent.nil?
      self
    end

    def replace_with(child)
      @parent.replace_child(self, child)
      self
    end

    def next_sibling
      return nil if @parent.nil?
      @parent[@parent.index(self) + 1]
    end

    def previous_sibling
      return nil if @parent.nil?
      ind = @parent.index(self)
      return nil if ind == 0
      @parent[ind - 1]
    end
    alias next_sibling_node next_sibling
    alias previous_sibling_node previous_sibling

    def document
      @parent.nil? ? nil : @parent.document
    end

    def parent? = false
  end

  class Text < Child
    attr_accessor :raw

    # REXML's signature: `respect_whitespace` false squeezes runs of spaces,
    # newlines and tabs; `raw` true keeps the string as written (checked).
    def initialize(arg, respect_whitespace = false, parent = nil, raw = nil)
      super()
      @raw = false
      @normalized = nil
      if arg.is_a?(Text)
        @string = arg.string_value.dup
        @raw = arg.raw
      else
        @string = arg.to_s.dup
      end
      @string = @string.squeeze(" \n\t") unless respect_whitespace
      @string = @string.gsub(/\r\n?/, "\n")
      @raw = raw unless raw.nil?
      Entities.check(@string) if @raw
      parent.add(self) unless parent.nil?
    end

    def string_value = @string

    def parent=(other)
      super(other)
      Entities.check(@string) if @raw && !@parent.nil?
      other
    end

    def node_type = :text

    def empty? = @string.empty?

    def clone = Text.new(self, true)

    def <<(to_append)
      @string = @string + to_append.gsub(/\r\n?/, "\n")
      @normalized = nil
      self
    end

    def <=>(other) = to_s <=> other.to_s

    def ==(other) = other.is_a?(Text) && to_s == other.to_s

    def to_s
      return @string if @raw
      @normalized ||= Entities.normalize(@string)
    end

    # REXML's Pretty formatter squeezes, in place, the String that `to_s`
    # returns: the node's own text, or its cached normalized form.
    def pretty_squeezed(str)
      if @raw
        @string = str
      else
        @normalized = str
      end
    end

    def value = Entities.unnormalize(@string)

    def value=(val)
      @string = val.gsub(/\r\n?/, "\n")
      @normalized = nil
      @raw = false
    end

    def inspect = @string.inspect
  end

  class CData < Text
    START = "<![CDATA["
    STOP = "]]>"

    def initialize(first, whitespace = true, parent = nil)
      super(first, whitespace, parent, false)
    end

    def clone = CData.new(self)

    def to_s = @string

    def value = @string

    def node_type = :cdata
  end

  class Comment < Child
    START = "<!--"
    STOP = "-->"
    attr_accessor :string

    def initialize(first, parent = nil)
      super()
      @string = first.is_a?(Comment) ? first.string : first.to_s
      parent.add(self) unless parent.nil?
    end

    def to_s = @string

    def clone = Comment.new(self)

    def ==(other) = other.is_a?(Comment) && other.string == @string

    def node_type = :comment
  end

  class Instruction < Child
    START = "<?"
    STOP = "?>"
    attr_accessor :target, :content

    def initialize(target, content = nil)
      super()
      @target = target
      @content = content.nil? ? nil : content.strip
    end

    def clone = Instruction.new(@target, @content)

    def ==(other) = other.is_a?(Instruction) && other.target == @target && other.content == @content

    def node_type = :processing_instruction

    def inspect = "<?p-i #{@target} ...?>"

    def to_s = Formatters::Default.new.write(self, +"")
  end

  class XMLDecl < Child
    attr_accessor :version, :standalone
    attr_reader :encoding, :writeencoding, :writethis

    def initialize(version = "1.0", encoding = nil, standalone = nil)
      super()
      @writethis = true
      @version = version.nil? ? "1.0" : version
      self.encoding = encoding
      @standalone = standalone
    end

    def self.default
      rv = XMLDecl.new("1.0")
      rv.nowrite
      rv
    end

    def encoding=(enc)
      if enc.nil?
        @encoding = "UTF-8"
        @writeencoding = false
      else
        unless enc.match?(/\Autf-8\z/i)
          raise NotImplementedError, "REXML encoding #{enc.inspect}: only UTF-8 is supported"
        end
        @encoding = enc.upcase
        @writeencoding = true
      end
      @writethis = true
    end

    def nowrite
      @writethis = false
    end

    def dowrite
      @writethis = true
    end

    def stand_alone? = @standalone

    def clone
      d = XMLDecl.new(@version, @writeencoding ? @encoding : nil, @standalone)
      d.nowrite unless @writethis
      d
    end

    def ==(other)
      other.is_a?(XMLDecl) && other.version == @version && other.encoding == @encoding && other.standalone == @standalone
    end

    def node_type = :xmldecl

    def inspect = "<?xml ... ?>"

    def to_s = Formatters::Default.new.write(self, +"")

    def write(writer, indent = -1, transitive = false, ie_hack = false)
      return nil unless @writethis
      writer << "<?xml version='" << @version << "'"
      writer << " encoding='" << @encoding << "'" if @writeencoding
      writer << " standalone='" << @standalone << "'" unless @standalone.nil?
      writer << "?>"
    end
  end

  # Namespace#name=: the expanded name split into prefix and local name.
  module Naming
    def self.split(name)
      if name.match?(NAME_WITHOUT_NAMESPACE)
        ["", name]
      elsif (m = name.to_s.match(NAMESPLIT))
        [m[1].nil? ? "" : m[1], m[2]]
      elsif name == ""
        [nil, nil]
      else
        raise ArgumentError, "name must be \#{PREFIX}:\#{LOCAL_NAME} or \#{LOCAL_NAME}: <#{name.inspect}>"
      end
    end
  end

  class Attribute
    attr_reader :expanded_name, :name, :prefix, :element

    # `normalized`: the value as written in XML (references intact), the way
    # the parser and Attributes#[]= hand it over.
    def initialize(first, normalized = nil)
      @expanded_name = first
      pn = Naming.split(first)
      @prefix = pn[0]
      @name = pn[1]
      @normalized = normalized.to_s
      @element = nil
    end
    alias local_name name

    def self.copy(attr)
      Attribute.new(attr.expanded_name, attr.to_s)
    end

    def ==(other) = other.is_a?(Attribute) && other.name == @name && other.value == value

    def namespace(arg = nil)
      arg = @prefix if arg.nil?
      arg == "" ? "" : @element.namespace(arg)
    end

    def to_s = @normalized

    def value = Entities.unnormalize(@normalized)

    def normalized=(new_normalized)
      @normalized = new_normalized
    end

    def to_string
      v = @normalized.include?("'") ? @normalized.gsub("'", "&apos;") : @normalized
      "#{@expanded_name}='#{v}'"
    end

    def element=(element)
      @element = element
      Entities.check(@normalized)
      self
    end

    def remove
      @element.attributes.delete(@name) unless @element.nil?
    end

    def document = @element.nil? ? nil : @element.document

    def node_type = :attribute

    def inspect = to_string
  end

  # An element's attributes. REXML's is a Hash keyed by local name whose
  # value is the Attribute, or a Hash of prefix to Attribute when several
  # share a local name; here each key holds the group, in REXML's order.
  class Attributes
    def initialize(element)
      @element = element
      @keys = []
      @groups = []
    end

    def group(name)
      i = @keys.index(name)
      i.nil? ? nil : @groups[i]
    end

    def get_attribute(name)
      return nil if name.nil?
      g = group(name)
      if g.nil?
        m = name.to_s.match(NAMESPLIT)
        return nil if m.nil? || m[1].nil?
        prefix = m[1]
        g2 = group(m[2])
        return nil if g2.nil?
        if g2.length == 1
          return g2[0] if g2[0].prefix == prefix
          return nil
        end
        g2.each { |a| return a if a.prefix == prefix }
        return nil
      end
      return g[0] if g.length == 1
      ep = @element.prefix
      g.each { |a| return a if a.prefix == ep }
      nil
    end

    def [](name)
      a = get_attribute(name)
      a.nil? ? nil : a.value
    end

    def []=(name, value)
      if value.nil?
        a = get_attribute(name)
        delete(a) unless a.nil?
        return nil
      end
      if value.is_a?(Attribute)
        store(value)
      else
        store(Attribute.new(name, Entities.normalize(value)))
      end
      value
    end

    def add(attribute)
      store(attribute)
      attribute
    end
    alias << add

    def store(attr)
      attr.element = @element
      i = @keys.index(attr.name)
      if i.nil?
        @keys << attr.name
        @groups << [attr]
        return attr
      end
      g = @groups[i]
      j = 0
      while j < g.length
        if g[j].prefix == attr.prefix
          g[j] = attr
          return attr
        end
        j += 1
      end
      g << attr
      attr
    end

    def delete(attribute)
      if attribute.is_a?(Attribute)
        name = attribute.name
        prefix = attribute.prefix
      else
        m = attribute.to_s.match(NAMESPLIT)
        prefix = m.nil? || m[1].nil? ? "" : m[1]
        name = m.nil? ? nil : m[2]
      end
      i = @keys.index(name)
      return @element if i.nil?
      g = @groups[i]
      if g.length > 1
        g.reject! { |a| a.prefix == prefix }
      else
        @keys.delete_at(i)
        @groups.delete_at(i)
      end
      @element
    end

    def delete_all(name)
      rv = to_a.select { |a| a.expanded_name == name }
      rv.each(&:remove)
      rv
    end

    def each_attribute
      @groups.each { |g| g.each { |a| yield a } }
    end

    def each
      to_a.each { |a| yield [a.expanded_name, a.value] }
    end

    def map
      to_a.map { |a| yield [a.expanded_name, a.value] }
    end

    def to_a
      out = []
      @groups.each { |g| g.each { |a| out << a } }
      out
    end

    def length = to_a.length
    alias size length

    def empty? = @keys.empty?

    def keys = @keys.dup

    def key?(name) = @keys.include?(name)
    alias has_key? key?
    alias include? key?
    alias member? key?

    def prefixes
      ns = []
      to_a.each { |a| ns << a.name if a.prefix == "xmlns" }
      ns
    end

    def namespaces
      h = {}
      to_a.each { |a| h[a.name] = a.value if a.prefix == "xmlns" || a.name == "xmlns" }
      h
    end
  end

  # What holds children: an Element or the Document.
  class Parent < Child
    def initialize
      super()
      @children = []
    end

    def add(object)
      object.parent = self
      @children << object
      object
    end
    alias push add
    alias << add

    def unshift(object)
      object.parent = self
      @children.unshift(object)
    end

    def delete(object)
      i = @children.index { |c| c.equal?(object) }
      return nil if i.nil?
      @children.reject! { |c| c.equal?(object) }
      object.parent = nil
      object
    end

    def each
      @children.each { |c| yield c }
    end
    alias each_child each

    def delete_at(index) = @children.delete_at(index)

    def [](index) = @children[index]

    def insert_before(child1, child2)
      ind = index(child1)
      child2.parent.delete(child2) unless child2.parent.nil?
      @children.insert(ind, child2)
      child2.parent = self
      self
    end

    def insert_after(child1, child2)
      ind = index(child1) + 1
      child2.parent.delete(child2) unless child2.parent.nil?
      @children.insert(ind, child2)
      child2.parent = self
      self
    end

    def children = @children.dup
    alias to_a children

    def index(child)
      i = @children.index { |c| c.equal?(child) }
      i.nil? ? @children.length - 1 : i
    end

    def size = @children.length
    alias length size

    def replace_child(to_replace, replacement)
      @children.map! { |c| c.equal?(to_replace) ? replacement : c }
      to_replace.parent = nil
      replacement.parent = self
    end

    def parent? = true

    def deep_clone
      cl = clone
      @children.each { |c| cl << (c.parent? ? c.deep_clone : c.clone) }
      cl
    end
  end

  class Element < Parent
    attr_reader :expanded_name, :name, :prefix, :attributes, :elements
    attr_accessor :context

    def initialize(name = "UNDEFINED", parent = nil)
      super()
      @elements = Elements.new(self)
      @attributes = Attributes.new(self)
      @context = nil
      self.name = name
      parent.add(self) unless parent.nil?
    end

    def name=(name)
      @expanded_name = name
      pn = Naming.split(name)
      @prefix = pn[0]
      @name = pn[1]
    end

    def clone
      el = Element.new(@expanded_name)
      @attributes.each_attribute { |a| el.attributes << Attribute.copy(a) }
      el
    end

    def inspect
      rv = +"<#{@expanded_name}"
      @attributes.each_attribute { |a| rv << " " << a.to_string }
      @children.empty? ? rv << "/>" : rv << "> ... </>"
    end

    def root_node = @parent.nil? ? self : @parent.root_node

    def root
      target = self
      until target.nil?
        return target.elements[1] if target.is_a?(Document)
        parent = target.parent
        return target if parent.nil? || parent.is_a?(Document)
        target = parent
      end
      nil
    end

    def document
      r = root
      r.nil? ? nil : r.parent
    end

    def whitespace = true

    def raw = nil

    def node_type = :element

    def prefixes
      ps = @parent.nil? ? [] : @parent.prefixes
      ps | @attributes.prefixes
    end

    def namespaces
      @parent.nil? ? @attributes.namespaces : @parent.namespaces.merge(@attributes.namespaces)
    end

    def namespace(prefix = nil)
      prefix = @prefix if prefix.nil?
      prefix = prefix == "" ? "xmlns" : prefix.delete_prefix("xmlns:")
      ns = namespaces[prefix]
      ns = "" if ns.nil? && prefix == "xmlns"
      ns
    end

    def add_namespace(prefix, uri = nil)
      if uri.nil?
        @attributes["xmlns"] = prefix
      else
        prefix = "xmlns:#{prefix}" unless prefix.match?(/^xmlns:/)
        @attributes[prefix] = uri
      end
      self
    end

    def has_name?(other)
      other.include?(":") ? fully_expanded_name == other : @name == other
    end

    def fully_expanded_name
      @prefix.nil? || @prefix.empty? ? @name : "#{@prefix}:#{@name}"
    end

    def add_element(element, attrs = nil)
      raise "First argument must be either an element name, or an Element object" if element.nil?
      el = @elements.add(element)
      attrs.each { |key, value| el.attributes[key] = value } unless attrs.nil?
      el
    end

    def delete_element(element) = @elements.delete(element)

    def has_elements? = !@elements.empty?

    def each_element(xpath = nil)
      @elements.each(xpath) { |e| yield e }
    end

    def get_elements(xpath) = @elements.to_a(xpath)

    def next_element
      element = next_sibling
      element = element.next_sibling until element.nil? || element.is_a?(Element)
      element
    end

    def previous_element
      element = previous_sibling
      element = element.previous_sibling until element.nil? || element.is_a?(Element)
      element
    end

    def has_text? = !text.nil?

    def text(path = nil)
      t = get_text(path)
      t.nil? ? nil : t.value
    end

    def get_text(path = nil)
      unless path.nil?
        element = @elements[path]
        return element.nil? ? nil : element.get_text
      end
      @children.each { |c| return c if c.is_a?(Text) }
      nil
    end

    def text=(text)
      t = text.nil? ? nil : (text.is_a?(Text) ? text : Text.new(text.to_s, true, nil, nil))
      old = get_text
      if t.nil?
        old.remove unless old.nil?
      elsif old.nil?
        add(t)
      else
        old.replace_with(t)
      end
      text
    end

    def add_text(text)
      if text.is_a?(Text)
        add(text)
        return self
      end
      last = @children.last
      if last.is_a?(Text)
        last << text
        return nil
      end
      add(Text.new(text, true, nil, nil))
      self
    end

    def [](name_or_index)
      return @attributes[name_or_index] if name_or_index.is_a?(String)
      return @attributes[name_or_index.to_s] if name_or_index.is_a?(Symbol)
      @children[name_or_index]
    end

    def attribute(name)
      @attributes.get_attribute(name)
    end

    def has_attributes? = !@attributes.empty?

    def add_attribute(key, value = nil)
      if key.is_a?(Attribute)
        @attributes << key
      else
        @attributes[key] = value
      end
    end

    def add_attributes(hash)
      hash.each { |key, value| @attributes[key] = value }
    end

    def delete_attribute(key)
      attr = @attributes.get_attribute(key)
      attr.remove unless attr.nil?
    end

    def texts = @children.select { |c| c.is_a?(Text) }.freeze

    def cdatas = @children.select { |c| c.is_a?(CData) }.freeze

    def comments = @children.select { |c| c.is_a?(Comment) }.freeze

    def instructions = @children.select { |c| c.is_a?(Instruction) }.freeze

    def to_s = Formatters::Default.new.write(self, +"")

    # REXML also warns that Element#write is deprecated.
    def write(output = $stdout, indent = -1, transitive = false, ie_hack = false)
      formatter = if indent > -1
        raise NotImplementedError, "REXML transitive formatting" if transitive
        Formatters::Pretty.new(indent, ie_hack)
      else
        Formatters::Default.new(ie_hack)
      end
      formatter.write(self, output)
    end
  end

  # The child elements of a parent, addressed as REXML's Elements are: by a
  # 1-based index, or by a path of names (`"a/b"`, `"p:a"`) and `*`.
  class Elements
    include Enumerable

    def initialize(parent)
      @element = parent
    end

    def parent = @element

    def [](index, name = nil)
      if index.is_a?(Integer)
        raise "index (#{index}) must be >= 1" if index < 1
        name = name[1, name.length - 2] if !name.nil? && (name[0] == "'" || name[0] == "\"")
        num = 0
        @element.each do |child|
          if child.is_a?(Element) && (name.nil? || child.has_name?(name))
            num += 1
            return child if num == index
          end
        end
        return nil
      end
      match(index.to_s).first
    end

    def []=(index, element)
      previous = self[index]
      if previous.nil?
        @element.add(element)
      else
        previous.replace_with(element)
      end
      previous
    end

    def empty? = @element.children.none? { |c| c.is_a?(Element) }

    def index(element)
      rv = 0
      @element.each do |child|
        if child.is_a?(Element)
          rv += 1
          return rv if child.equal?(element)
        end
      end
      -1
    end

    def delete(element)
      if element.is_a?(Element)
        @element.delete(element)
      else
        el = self[element]
        el.remove unless el.nil?
      end
    end

    def delete_all(xpath)
      rv = match(xpath)
      rv.each { |e| @element.delete(e) }
      rv
    end

    def add(element = nil)
      el = element.is_a?(Element) ? element : Element.new(element.nil? ? "" : element.to_s)
      @element << el
      el.context = @element.context
      el
    end
    alias << add

    def each(xpath = nil)
      match(xpath.nil? ? "*" : xpath).each { |e| yield e }
    end

    def collect(xpath = nil)
      match(xpath.nil? ? "*" : xpath).map { |e| yield e }
    end

    def inject(xpath = nil, initial = nil)
      acc = initial
      first = true
      match(xpath.nil? ? "*" : xpath).each do |e|
        if first && acc.nil?
          acc = e
        else
          acc = yield(acc, e)
        end
        first = false
      end
      acc
    end

    def size = @element.children.count { |c| c.is_a?(Element) }

    def to_a(xpath = nil) = match(xpath.nil? ? "*" : xpath)

    private

    # REXML's XPath for a child path: a name test without a prefix matches
    # an element of that local name in the element's default namespace, a
    # prefixed one an element in the namespace its prefix is bound to.
    def match(path)
      steps = path.split("/", -1)
      unless !steps.empty? && steps.all? { |s| s.match?(XPATH_STEP) }
        raise NotImplementedError, "REXML XPath #{path.inspect}: only a child path of names and * is supported"
      end
      found = [@element]
      steps.each do |step|
        nxt = []
        found.each do |node|
          node.children.each do |c|
            next unless c.is_a?(Element)
            next if nxt.any? { |n| n.equal?(c) }
            nxt << c if step_match?(step, c)
          end
        end
        found = nxt
      end
      found
    end

    def step_match?(step, el)
      return true if step == "*"
      i = step.index(":")
      prefix = i.nil? ? "" : step[0, i]
      local = i.nil? ? step : step[i + 1, step.length - i - 1]
      el.name == local && el.namespace == el.namespace(prefix)
    end
  end

  class Document < Element
    def initialize(source = nil, context = nil)
      super()
      unless context.nil? || context.empty?
        raise NotImplementedError, "REXML Document context options are not supported"
      end
      return if source.nil? || source == ""
      unless source.is_a?(String)
        raise NotImplementedError, "REXML Document source: pass a String (File.read the file)"
      end
      TreeParser.new(source, self).parse
    end

    def expanded_name = ""

    def name = ""

    def node_type = :document

    def clone = Document.new

    def add(child)
      if child.is_a?(XMLDecl)
        if @children[0].is_a?(XMLDecl)
          @children[0] = child
        else
          @children.unshift(child)
        end
        child.parent = self
        child
      elsif child.is_a?(DocType)
        insert_before_index = @children.find_index { |x| x.is_a?(Element) || x.is_a?(DocType) }
        if insert_before_index.nil?
          @children << child
        elsif @children[insert_before_index].is_a?(DocType)
          @children[insert_before_index] = child
        else
          # REXML's `@children[insert_before_index - 1, 0] = child`: one slot early, and from the end when that is -1
          at = insert_before_index - 1
          at += @children.length if at < 0
          @children.insert(at, child)
        end
        child.parent = self
        child
      else
        rv = super(child)
        raise "attempted adding second root element to document" if @elements.size > 1
        rv
      end
    end
    alias << add

    def add_element(arg = nil, arg2 = nil)
      rv = super(arg, arg2)
      raise "attempted adding second root element to document" if @elements.size > 1
      rv
    end

    def root = @elements[1]

    def doctype = @children.find { |item| item.is_a?(DocType) }

    def xml_decl
      rv = @children[0]
      return rv if rv.is_a?(XMLDecl)
      d = XMLDecl.default
      @children.unshift(d)
      d
    end

    def version = xml_decl.version

    def encoding = xml_decl.encoding

    def stand_alone? = xml_decl.stand_alone?

    def to_s = Formatters::Default.new.write(self, +"")

    def write(output = $stdout, indent = -1, transitive = false, ie_hack = false, encoding = nil)
      encoding = xml_decl.encoding if encoding.nil?
      raise NotImplementedError, "REXML encoding #{encoding.inspect}: only UTF-8 is supported" unless encoding == "UTF-8"
      super(output, indent, transitive, ie_hack)
    end
  end

  # The external ID a DOCTYPE writes after its name.
  class ReferenceWriter
    # A Document context (and so `prologue_quote: :apostrophe`) is not
    # supported, so the default quote is always `"`.
    def initialize(id_type, public_id_literal, system_literal)
      @id_type = id_type
      @public_id_literal = public_id_literal
      @system_literal = system_literal
      @default_quote = "\""
    end

    def write(output)
      output << " #{@id_type}"
      unless @public_id_literal.nil?
        quote = @public_id_literal.include?("'") ? "\"" : @default_quote
        output << " #{quote}#{@public_id_literal}#{quote}"
      end
      unless @system_literal.nil?
        quote = if @system_literal.include?("'")
          "\""
        elsif @system_literal.include?("\"")
          "'"
        else
          @default_quote
        end
        output << " #{quote}#{@system_literal}#{quote}"
      end
      output
    end
  end

  # A DOCTYPE a program builds and writes. `DocType.new("svg PUBLIC ...")`
  # keeps the whole string as the name, as REXML does.
  class DocType < Parent
    START = "<!DOCTYPE"
    STOP = ">"
    SYSTEM = "SYSTEM"
    PUBLIC = "PUBLIC"

    attr_reader :name, :external_id, :long_name, :uri

    # REXML's second argument is the external ID after a String name, and
    # the parent after a DocType or an Array; the parent form is not here.
    def initialize(first, parent = nil)
      super()
      @long_name = nil
      @uri = nil
      if first.is_a?(String)
        @name = first
        @external_id = parent
      elsif first.is_a?(DocType)
        raise NotImplementedError, "REXML DocType.new(doctype, parent): add the DocType to its parent" unless parent.nil?
        @name = first.name
        @external_id = first.external_id
        @long_name = first.long_name
        @uri = first.uri
      elsif first.is_a?(Array)
        raise NotImplementedError, "REXML DocType.new(array, parent): add the DocType to its parent" unless parent.nil?
        @name = first[0]
        @external_id = first[1]
        @long_name = first[2]
        @uri = first[3]
      else
        raise NotImplementedError, "REXML DocType from a Source: DOCTYPE parsing is not supported"
      end
    end

    def node_type = :doctype

    def clone = DocType.new(self)

    def to_s = Formatters::Default.new.write(self, +"")

    def write(output, indent = 0, transitive = false, ie_hack = false)
      f = Formatters::Default.new
      output << "  " * indent unless indent < 1
      output << START
      output << " "
      output << @name
      unless @external_id.nil?
        ReferenceWriter.new(@external_id, @long_name, @uri).write(output)
      end
      unless @children.empty?
        output << " ["
        @children.each do |child|
          output << "\n"
          f.write(child, output)
        end
        output << "\n]"
      end
      output << STOP
    end

    def context = @parent.nil? ? nil : @parent.context

    def public
      if @external_id == "SYSTEM"
        nil
      elsif @external_id == "PUBLIC"
        @long_name
      end
    end

    def system
      if @external_id == "SYSTEM"
        @long_name
      elsif @external_id == "PUBLIC"
        @uri
      end
    end
  end

  module Formatters
    # REXML's Default formatter: the document as it is, with ie_hack's space
    # before an empty tag's `/`.
    class Default
      def initialize(ie_hack = false)
        @ie_hack = ie_hack
      end

      def write(node, output)
        if node.is_a?(Document)
          node.xml_decl
          write_document(node, output)
        elsif node.is_a?(Element)
          write_element(node, output)
        elsif node.is_a?(Attribute)
          output << node.to_string
        elsif node.is_a?(Instruction)
          write_instruction(node, output)
        elsif node.is_a?(DocType) || node.is_a?(XMLDecl)
          node.write(output)
        elsif node.is_a?(Comment)
          write_comment(node, output)
        elsif node.is_a?(CData)
          write_cdata(node, output)
        elsif node.is_a?(Text)
          write_text(node, output)
        else
          raise Exception.new("XML FORMATTING ERROR")
        end
        output
      end

      def write_document(node, output)
        node.children.each { |child| write(child, output) }
      end

      # REXML sorts with `sort_by { |attr| attr.name }`, whose order for equal
      # local names is not promised; source order keeps it deterministic.
      def write_element(node, output)
        output << "<#{node.expanded_name}"
        unless node.attributes.empty?
          node.attributes.to_a.each_with_index.sort_by { |a, i| [a.name, i] }.each do |attr, _i|
            output << " "
            output << attr.to_string
          end
        end
        if node.children.empty?
          output << " " if @ie_hack
          output << "/"
        else
          output << ">"
          node.children.each { |child| write(child, output) }
          output << "</#{node.expanded_name}"
        end
        output << ">"
      end

      def write_text(node, output)
        output << node.to_s
      end

      def write_comment(node, output)
        output << Comment::START
        output << node.to_s
        output << Comment::STOP
      end

      def write_cdata(node, output)
        output << CData::START
        output << node.to_s
        output << CData::STOP
      end

      def write_instruction(node, output)
        output << Instruction::START
        output << node.target
        content = node.content
        unless content.nil?
          output << " "
          output << content
        end
        output << Instruction::STOP
      end
    end

    # REXML's Pretty formatter: each node on its own line, indented, texts
    # squeezed and wrapped at `width`; `compact` keeps an element whose
    # children are all texts on one line when it fits.
    class Pretty < Default
      attr_accessor :compact, :width

      def initialize(indentation = 2, ie_hack = false)
        @indentation = indentation
        @level = 0
        @ie_hack = ie_hack
        @width = 80
        @compact = false
      end

      def write_element(node, output)
        output << " " * @level
        output << "<#{node.expanded_name}"
        unless node.attributes.empty?
          node.attributes.each_attribute do |attr|
            output << " "
            output << attr.to_string
          end
        end
        if node.children.empty?
          output << " " if @ie_hack
          output << "/"
        else
          output << ">"
          skip = false
          if compact
            if node.children.all? { |c| c.is_a?(Text) }
              string = +""
              old_level = @level
              @level = 0
              node.children.each { |child| write(child, string) }
              @level = old_level
              if string.length < @width
                output << string
                skip = true
              end
            end
          end
          unless skip
            output << "\n"
            @level += @indentation
            node.children.each do |child|
              next if child.is_a?(Text) && child.to_s.strip.length == 0
              write(child, output)
              output << "\n"
            end
            @level -= @indentation
            output << " " * @level
          end
          output << "</#{node.expanded_name}"
        end
        output << ">"
      end

      def write_text(node, output)
        s = node.to_s.gsub(/\s/, " ").squeeze(" ")
        node.pretty_squeezed(s)
        s = wrap(s, @width - @level)
        s = indent_text(s, @level, " ", true)
        output << (" " * @level + s)
      end

      def write_comment(node, output)
        output << " " * @level
        super(node, output)
      end

      def write_cdata(node, output)
        output << " " * @level
        super(node, output)
      end

      # Default#write has given the document an XMLDecl as its first child.
      def write_document(node, output)
        node.children.each do |child|
          next if child.instance_of?(Text)
          first = node.children[0]
          unless child == first || child.instance_of?(Text) ||
                 (child == node.children[1] && first.is_a?(XMLDecl) && !first.writethis)
            output << "\n"
          end
          write(child, output)
        end
      end

      def indent_text(string, level = 1, style = "\t", indentfirstline = true)
        return string if level < 0
        string.gsub(/\n/, "\n#{style * level}")
      end

      def wrap(string, width)
        parts = []
        while string.length > width
          place = string.rindex(" ", width)
          break if place.nil?
          parts << string[0, place]
          string = string[place + 1, string.length - place - 1]
        end
        parts << string
        parts.join("\n")
      end
    end
  end

  # REXML's IOSource over a String: the parser sees the document as it is
  # read, in chunks ending at `>` (or at the terminator it is looking for),
  # and an error reports how far the reading got.
  class Source
    def initialize(src)
      @src = src
      @len = src.length
      n = 0
      bytes = 0
      while n < @len && bytes + @src[n].bytesize <= 3
        bytes += @src[n].bytesize
        n += 1
      end
      @io = n
      start = @src.start_with?("﻿") ? 1 : 0
      @pending = start < n
      @pos = start
      @loaded = start
      @dead = false
    end

    def position = @pos

    def position=(pos)
      @pos = pos
    end

    def buffer = @src[@pos, @loaded - @pos]

    def empty? = @pos >= @loaded && (@dead || @io >= @len)

    def error_position = @src[0, @io].bytesize

    def error_line
      return 0 if @io == 0
      head = @src[0, @io]
      head.count("\n") + (head.end_with?("\n") ? 0 : 1)
    end

    # IOSource#readline: the next chunk up to `term`; false at the end.
    def readline(term)
      if @pending
        @pending = false
        io_read(term) if @io < @len
        @loaded = @io
        return true
      end
      return false if @io >= @len
      io_read(term)
      @loaded = @io
      true
    end

    def io_read(term)
      i = @src.index(term, @io)
      @io = i.nil? ? @len : i + term.length
    end

    def read(term = ">", min_bytes = 1)
      start = @loaded
      unless readline(term)
        @dead = true
        return false
      end
      while @src[start, @loaded - start].bytesize < min_bytes
        break unless readline(term)
      end
      true
    end

    def ensure_buffer
      read if @pos >= @loaded && !@dead
    end

    def read_until(term)
      idx = nil
      loop do
        idx = @src.index(term, @pos)
        idx = nil if !idx.nil? && idx + term.length > @loaded
        break unless idx.nil?
        break if @dead || @io >= @len
        readline(term)
      end
      if idx.nil?
        rest = buffer
        @pos = @loaded
        return rest
      end
      str = @src[@pos, idx + term.length - @pos]
      @pos = idx + term.length
      read if @pos >= @loaded && !@dead && @io < @len
      str
    end

    # A literal at the current position, never reading more.
    def match_str?(str, cons = false)
      return false if @pos + str.length > @loaded
      return false unless @src[@pos, str.length] == str
      @pos += str.length if cons
      true
    end

    # A pattern (anchored with \A) at the current position, reading more
    # until it matches or the input ends.
    def match(pattern, cons = false)
      min_bytes = 1
      loop do
        m = buffer.match(pattern)
        unless m.nil?
          @pos += m[0].length if cons
          return m
        end
        return nil if @dead
        return nil unless read(">", min_bytes)
        min_bytes *= 2
      end
    end

    def match?(pattern, cons = false) = !match(pattern, cons).nil?

    def skip_spaces
      m = buffer.match(/\A\s+/)
      return false if m.nil?
      @pos += m[0].length
      true
    end

    def peek = @pos < @loaded ? @src[@pos] : nil

    def advance
      @pos += 1
    end
  end

  # BaseParser (without DOCTYPE) and TreeParser, building into a Document.
  class TreeParser
    PNCNAME = "[[:alpha:]_][-[:alnum:]._]*"
    PQNAME_STR = "(?:(#{PNCNAME}):)?(#{PNCNAME})"
    QNAME = /\A((?:(#{PNCNAME}):)?(#{PNCNAME}))/
    TAG_PATTERN = /\A(#{PQNAME_STR})\s*/m
    CLOSE_PATTERN = /\A(#{PQNAME_STR})\s*>/m
    EQUAL_PATTERN = /\A\s*=\s*/m
    NAME_PATTERN = /\A[\w:][-\w.:]*/m
    XML_NS = "http://www.w3.org/XML/1998/namespace"

    def initialize(src, doc)
      @source = Source.new(src)
      @doc = doc
      @context = doc
      @tags = []
      @have_root = false
      @status = nil
      @closed = nil
      @version = nil
      @prefixes = []
      @namespaces = { "xml" => XML_NS }
      @restore_stack = []
      @source.ensure_buffer
    end

    # TreeParser#parse: an error from building the tree (a RuntimeError or
    # ArgumentError) becomes a ParseException quoting it.
    def parse
      loop do
        begin
          return @doc if pull_event == :end_document
        rescue ParseException => e
          raise e
        rescue => e
          raise ParseException.new(e.message, @source, e)
        end
      end
    end

    private

    def error(message)
      ParseException.new(message, @source)
    end

    # One pull event, applied to the tree; answers its kind.
    def pull_event
      unless @closed.nil?
        @closed = nil
        @context = @context.parent
        return :end_element
      end
      if @source.empty?
        unless @tags.empty?
          raise error("Missing end tag for '/#{@tags.join("/")}'")
        end
        raise error("Malformed XML: No root element") unless @status == :in_element
        return :end_document
      end
      @source.ensure_buffer
      if @status.nil?
        if @source.match_str?("<?", true)
          return process_instruction
        elsif @source.match_str?("<!", true)
          if @source.match_str?("--", true)
            @context.add(Comment.new(process_comment))
            return :comment
          elsif @source.match_str?("DOCTYPE", true)
            raise NotImplementedError, "REXML DOCTYPE declarations are not supported"
          else
            raise error("Invalid XML")
          end
        end
      end
      start_position = @source.position
      if @source.match_str?("<", true)
        @source.ensure_buffer
        if @source.match_str?("/", true)
          @restore_stack.pop
          last_tag = @tags.pop
          md = @source.match(CLOSE_PATTERN, true)
          if !md.nil? && last_tag.nil?
            raise error("Unexpected top-level end tag (got '#{md[1]}')")
          end
          if md.nil? || last_tag != md[1]
            message = "Missing end tag for '#{last_tag}'"
            message += " (got '#{md[1]}')" unless md.nil?
            @source.position = start_position if md.nil?
            raise error(message)
          end
          @context = @context.parent
          return :end_element
        elsif @source.match_str?("!", true)
          if @source.match_str?("--", true)
            @context.add(Comment.new(process_comment))
            return :comment
          elsif @source.match_str?("[CDATA[", true)
            text = @source.read_until("]]>")
            raise error("Malformed CDATA: Missing end ']]>'") unless text.end_with?("]]>")
            @context.add(CData.new(text[0, text.length - 3]))
            return :cdata
          else
            raise error("Malformed node: Started with '<!' but not a comment nor CDATA")
          end
        elsif @source.match_str?("?", true)
          return process_instruction
        else
          md = @source.match(TAG_PATTERN, true)
          if md.nil?
            @source.position = start_position
            raise error("malformed XML: missing tag start")
          end
          tag = md[1]
          @status = :in_element
          @prefixes = []
          @prefixes << md[2] unless md[2].nil?
          @restore_stack.push({})
          attrs, closed = parse_attributes
          @prefixes.each do |prefix|
            raise UndefinedNamespaceException.new(prefix) unless @namespaces.key?(prefix)
          end
          if closed
            @closed = tag
            pop_namespaces_restore
          else
            if @tags.empty? && @have_root
              raise error("Malformed XML: Extra tag at the end of the document (got '<#{tag}')")
            end
            @tags.push(tag)
          end
          @have_root = true
          start_element(tag, attrs)
          return :start_element
        end
      end
      text = @source.read_until("<")
      if text.end_with?("<")
        text = text[0, text.length - 1]
        @source.position = @source.position - 1
      end
      if @tags.empty?
        unless text.match?(/\A\s*\z/)
          if @have_root
            raise error("Malformed XML: Extra content at the end of the document (got '#{text}')")
          else
            raise error("Malformed XML: Content at the start of the document (got '#{text}')")
          end
        end
        return :text if @have_root
      end
      add_text(text)
      :text
    end

    def start_element(tag, attrs)
      el = Element.new(tag)
      @context.add(el)
      @context = el
      attrs.each { |pair| el.attributes.add(Attribute.new(pair[0], pair[1])) }
    end

    def add_text(text)
      last = @context[-1]
      if last.instance_of?(Text)
        last << text
      else
        @context.add(Text.new(text, true, nil, true))
      end
    end

    def pop_namespaces_restore
      restore = @restore_stack.pop
      restore.each do |prefix, uri|
        if uri.nil?
          @namespaces.delete(prefix)
        else
          @namespaces[prefix] = uri
        end
      end
    end

    def add_namespace(prefix, uri)
      @restore_stack.last[prefix] = @namespaces[prefix]
      @namespaces[prefix] = uri
    end

    def process_comment
      text = @source.read_until("-->")
      raise error("Unclosed comment: Missing end '-->'") unless text.end_with?("-->")
      text = text[0, text.length - 3]
      raise error("Malformed comment") if text.include?("--") || text.end_with?("-")
      text
    end

    def process_instruction
      md = @source.match(NAME_PATTERN, true)
      if md.nil?
        if @source.match?(/\A\S/m)
          raise error("Malformed XML: Invalid processing instruction node: invalid name")
        end
        raise error("Malformed XML: Invalid processing instruction node: name is missing")
      end
      name = md[0]
      return xml_declaration if name == "xml"
      content = nil
      if @source.skip_spaces
        start_position = @source.position
        content = @source.read_until("?>")
        unless content.end_with?("?>")
          @source.position = start_position
          raise error("Malformed XML: Unclosed processing instruction: <#{name}>")
        end
        content = content[0, content.length - 2]
      else
        raise error("Malformed XML: Unclosed processing instruction: <#{name}>") unless @source.match_str?("?>", true)
      end
      @context.add(Instruction.new(name, content))
      :processing_instruction
    end

    def xml_declaration
      raise error("Malformed XML: XML declaration is duplicated") unless @version.nil?
      raise error("Malformed XML: XML declaration is not at the start") unless @status.nil?
      raise error("Malformed XML: XML declaration misses spaces before version") unless @source.skip_spaces
      raise error("Malformed XML: XML declaration misses version") unless @source.match_str?("version", true)
      @version = attribute_value_with_equal("xml")
      encoding = nil
      standalone = nil
      if @source.skip_spaces
        if @source.match_str?("encoding", true)
          encoding = attribute_value_with_equal("xml")
          unless @source.skip_spaces
            raise error("Malformed XML: Unclosed XML declaration") unless @source.match_str?("?>", true)
            return add_xmldecl(encoding, nil)
          end
        end
        if @source.match_str?("standalone", true)
          standalone = attribute_value_with_equal("xml")
          unless standalone == "yes" || standalone == "no"
            raise error("Malformed XML: XML declaration standalone is not yes or no : <#{standalone}>")
          end
        end
        @source.skip_spaces
      end
      raise error("Malformed XML: Unclosed XML declaration") unless @source.match_str?("?>", true)
      add_xmldecl(encoding, standalone)
    end

    def add_xmldecl(encoding, standalone)
      @doc.add(XMLDecl.new(@version, encoding, standalone))
      :xmldecl
    end

    def attribute_value_with_equal(name)
      raise error("Missing attribute equal: <#{name}>") unless @source.match?(EQUAL_PATTERN, true)
      quote = @source.peek
      unless quote == "'" || quote == "\""
        raise error("Missing attribute value start quote: <#{name}>")
      end
      @source.advance
      start_position = @source.position
      value = @source.read_until(quote)
      unless value.end_with?(quote)
        @source.position = start_position
        raise error("Missing attribute value end quote: <#{name}>: <#{quote}>")
      end
      value[0, value.length - 1]
    end

    # Answers [[name, value], ...] in source order and whether the tag
    # closed itself.
    def parse_attributes
      attributes = []
      expanded_names = {}
      loop do
        return [attributes, false] if @source.match_str?(">", true)
        return [attributes, true] if @source.match_str?("/>", true)
        md = @source.match(QNAME, true)
        if md.nil?
          first = @source.buffer.split(%r{[/>\s]}).first
          raise error("Invalid attribute name: <#{first}>")
        end
        name = md[1]
        prefix = md[2]
        local_part = md[3]
        value = attribute_value_with_equal(name)
        @source.skip_spaces
        if prefix == "xmlns"
          if local_part == "xml"
            if value != XML_NS
              raise error("The 'xml' prefix must not be bound to any other namespace (http://www.w3.org/TR/REC-xml-names/#ns-decl)")
            end
          elsif local_part == "xmlns"
            raise error("The 'xmlns' prefix must not be declared (http://www.w3.org/TR/REC-xml-names/#ns-decl)")
          end
          add_namespace(local_part, value)
        elsif !prefix.nil?
          @prefixes << prefix unless prefix == "xml" || @prefixes.include?(prefix)
        end
        if attributes.any? { |pair| pair[0] == name }
          raise error("Duplicate attribute #{name.inspect}")
        end
        unless prefix == "xmlns"
          uri = prefix.nil? ? nil : @namespaces[prefix]
          key = (uri.nil? ? "N" : "S#{uri}") + "\u0000" + local_part
          existing_prefix = expanded_names[key]
          unless existing_prefix.nil?
            raise error("Namespace conflict in adding attribute \"#{local_part}\": Prefix \"#{existing_prefix}\" = \"#{uri}\" and prefix \"#{prefix}\" = \"#{uri}\"")
          end
          expanded_names[key] = prefix.nil? ? "" : prefix
        end
        attributes << [name, value]
      end
    end
  end
end
