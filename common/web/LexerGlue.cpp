/*
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * LexerGlue.h
 * Captures errors while parsing the schema.
 * Copyright (C) 2014 Simon Newton
 */

#include <string>

#include "common/web/LexerGlue.h"

namespace ola {
namespace web {

namespace {

bool FooExtractDigits(const char **input, uint64_t *i,
                      unsigned int *leading_zeros = NULL) {
  *i = 0;
  bool at_start = true;
  unsigned int zeros = 0;
  while (isdigit(**input)) {
    if (at_start && **input == '0') {
      zeros++;
    } else if (at_start) {
      at_start = false;
    }

    *i *= 10;
    *i += **input - '0';
    (*input)++;
  }
  if (leading_zeros) {
    *leading_zeros = zeros;
  }
  return true;
}

}

LexerGlue::LexerGlue(JsonParserInterface *parser)
    : m_parser(parser) {
  m_expect_stack.push(START);
}

void LexerGlue::String(const char *text) {
  Expect &state = m_expect_stack.top();

  if (state == OBJECT_KEY) {
    m_parser->ObjectKey(text);
    state = OBJECT_COLON;
  } else if (state == OBJECT_ELEMENT) {
    m_parser->String(text);
    state = OBJECT_COMMA;
  } else if (state == ARRAY_ELEMENT) {
    m_parser->String(text);
    state = ARRAY_COMMA;
  } else {
    // bad state!
  }
}

void LexerGlue::Bool(bool value) {
  Expect &state = m_expect_stack.top();

  if (state == OBJECT_ELEMENT) {
    m_parser->Bool(value);
    state = OBJECT_COMMA;
  } else if (state == ARRAY_ELEMENT) {
    m_parser->Bool(value);
    state = ARRAY_COMMA;
  } else {
    // bad state!
  }
}

void LexerGlue::Int(const char *text) {
  uint64_t value;
  FooExtractDigits(&text, &value);

  Expect &state = m_expect_stack.top();

  if (state == OBJECT_ELEMENT) {
    m_parser->Number(value);
    state = OBJECT_COMMA;
  } else if (state == ARRAY_ELEMENT) {
    m_parser->Number(value);
    state = ARRAY_COMMA;
  } else {
    // bad state!
  }
}

void LexerGlue::Fractional(const char *text) {
  uint64_t value;
  FooExtractDigits(&text, &value);

}

void LexerGlue::Exponent(const char *text) {
  uint64_t value;
  FooExtractDigits(&text, &value);

}

void LexerGlue::Null() {
  Expect &state = m_expect_stack.top();

  if (state == OBJECT_ELEMENT) {
    m_parser->Null();
    state = OBJECT_COMMA;
  } else if (state == ARRAY_ELEMENT) {
    m_parser->Null();
    state = ARRAY_COMMA;
  } else {
    // bad state!
  }
}

void LexerGlue::OpenArray() {
  Expect &state = m_expect_stack.top();

  if (state == START) {
    m_parser->OpenArray();
    state = ARRAY_ELEMENT;
  } else if (state == ARRAY_ELEMENT) {
    state = ARRAY_COMMA;
    m_expect_stack.push(ARRAY_ELEMENT);
    m_parser->OpenArray();
  } else if (state == OBJECT_ELEMENT) {
    state = OBJECT_COMMA;
    m_expect_stack.push(ARRAY_ELEMENT);
    m_parser->OpenArray();
  } else {

  }
}

void LexerGlue::CloseArray() {
  Expect &state = m_expect_stack.top();
  if (state == ARRAY_COMMA) {
    m_parser->CloseArray();
    m_expect_stack.pop();
  } else if (state == ARRAY_ELEMENT) {
    m_parser->CloseArray();
    m_expect_stack.pop();
  } else {

  }
}

void LexerGlue::OpenObject() {
  Expect &state = m_expect_stack.top();

  if (state == START) {
    m_parser->OpenObject();
    state = OBJECT_KEY;
  } else if (state == ARRAY_ELEMENT) {
    state = ARRAY_COMMA;
    m_expect_stack.push(OBJECT_KEY);
    m_parser->OpenObject();
  } else if (state == OBJECT_ELEMENT) {
    state = OBJECT_COMMA;
    m_expect_stack.push(OBJECT_KEY);
    m_parser->OpenObject();
  } else {

  }
}

void LexerGlue::CloseObject() {
  Expect &state = m_expect_stack.top();
  if (state == OBJECT_KEY) {
    m_parser->CloseObject();
    m_expect_stack.pop();
  } else if (state == OBJECT_COMMA) {
    m_parser->CloseObject();
    m_expect_stack.pop();
  } else {

  }
}

void LexerGlue::Comma() {
  Expect &state = m_expect_stack.top();
  if (state == OBJECT_COMMA) {
    state = OBJECT_KEY;
  } else if (state == ARRAY_COMMA) {
    state = ARRAY_ELEMENT;
  } else {
    // bad state
  }
}

void LexerGlue::Colon() {
  Expect &state = m_expect_stack.top();
  if (state == OBJECT_COLON) {
    state = OBJECT_ELEMENT;
  } else {
    // bad state
  }
}
}  // namespace web
}  // namespace ola
