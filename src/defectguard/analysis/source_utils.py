from __future__ import annotations


def mask_comments_and_strings(text: str) -> str:
    """屏蔽注释与字符串内容，同时保留换行和字符位置。"""

    output = list(text)
    index = 0
    state = "code"
    quote = ""
    while index < len(text):
        current = text[index]
        following = text[index + 1] if index + 1 < len(text) else ""
        if state == "code":
            if current == "/" and following == "/":
                output[index] = output[index + 1] = " "
                index += 2
                state = "line-comment"
                continue
            if current == "/" and following == "*":
                output[index] = output[index + 1] = " "
                index += 2
                state = "block-comment"
                continue
            if current in {'"', "'"}:
                quote = current
                output[index] = " "
                index += 1
                state = "string"
                continue
        elif state == "line-comment":
            if current == "\n":
                state = "code"
            else:
                output[index] = " "
        elif state == "block-comment":
            if current == "*" and following == "/":
                output[index] = output[index + 1] = " "
                index += 2
                state = "code"
                continue
            if current != "\n":
                output[index] = " "
        elif state == "string":
            if current == "\\" and following:
                if current != "\n":
                    output[index] = " "
                if following != "\n":
                    output[index + 1] = " "
                index += 2
                continue
            if current == quote:
                output[index] = " "
                index += 1
                state = "code"
                continue
            if current != "\n":
                output[index] = " "
        index += 1
    return "".join(output)


def line_number_at(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1

