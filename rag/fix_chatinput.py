#!/usr/bin/env python3
"""
改进 ChatInput 组件：添加错误处理和调试日志
"""
import json

json_file = r"D:\迅雷下载\MiniOB2025\MiniOB_2025_Competition_Version\rag\model.json"

with open(json_file, "r", encoding="utf-8") as f:
    data = json.load(f)

# 找到 ChatInput 组件
for node in data["data"]["nodes"]:
    if node["data"]["id"] == "ChatInput-24Peo":
        # 完整替换代码，添加错误处理和日志
        new_code = '''import os

from langflow.base.data.utils import IMG_FILE_TYPES, TEXT_FILE_TYPES
from langflow.base.io.chat import ChatComponent
from langflow.inputs.inputs import BoolInput
from langflow.io import (
    DropdownInput,
    FileInput,
    MessageTextInput,
    MultilineInput,
    Output,
)
from langflow.schema.message import Message
from langflow.utils.constants import (
    MESSAGE_SENDER_AI,
    MESSAGE_SENDER_NAME_USER,
    MESSAGE_SENDER_USER,
)
import requests


class ChatInput(ChatComponent):
    display_name = "Chat Input"
    description = "Get chat inputs from the Playground."
    documentation: str = "https://docs.langflow.org/components-io#chat-input"
    icon = "MessagesSquare"
    name = "ChatInput"
    minimized = True

    inputs = [
        MultilineInput(
            name="input_value",
            display_name="Input Text",
            value="",
            info="Message to be passed as input.",
            input_types=[],
        ),
        BoolInput(
            name="should_store_message",
            display_name="Store Messages",
            info="Store the message in the history.",
            value=True,
            advanced=True,
        ),
        DropdownInput(
            name="sender",
            display_name="Sender Type",
            options=[MESSAGE_SENDER_AI, MESSAGE_SENDER_USER],
            value=MESSAGE_SENDER_USER,
            info="Type of sender.",
            advanced=True,
        ),
        MessageTextInput(
            name="sender_name",
            display_name="Sender Name",
            info="Name of the sender.",
            value=MESSAGE_SENDER_NAME_USER,
            advanced=True,
        ),
        MessageTextInput(
            name="session_id",
            display_name="Session ID",
            info="The session ID of the chat. If empty, the current session ID parameter will be used.",
            advanced=True,
        ),
        FileInput(
            name="files",
            display_name="Files",
            file_types=TEXT_FILE_TYPES + IMG_FILE_TYPES,
            info="Files to be sent with the message.",
            advanced=True,
            is_list=True,
            temp_file=True,
        ),
        MessageTextInput(
            name="background_color",
            display_name="Background Color",
            info="The background color of the icon.",
            advanced=True,
        ),
        MessageTextInput(
            name="chat_icon",
            display_name="Icon",
            info="The icon of the message.",
            advanced=True,
        ),
        MessageTextInput(
            name="text_color",
            display_name="Text Color",
            info="The text color of the name",
            advanced=True,
        ),
    ]
    outputs = [
        Output(display_name="Chat Message", name="message", method="message_response"),
    ]

    async def message_response(self) -> Message:
        print("=" * 60)
        print("[ChatInput] Starting message_response")

        background_color = self.background_color
        text_color = self.text_color
        icon = self.chat_icon
        text = self.input_value

        print(f"[ChatInput] input_value: '{text}'")
        print(f"[ChatInput] input_value type: {type(text)}")
        print(f"[ChatInput] input_value bool: {bool(text)}")

        if not text:
            print("[ChatInput] input_value is empty, fetching from test server")
            url = os.getenv("QA_SERVER_GET_QUESTION_URL")
            print(f"[ChatInput] QA_SERVER_GET_QUESTION_URL: {url}")

            if not url:
                print("[ChatInput] ERROR: QA_SERVER_GET_QUESTION_URL not set!")
                text = "ERROR: QA_SERVER_GET_QUESTION_URL not configured"
            else:
                try:
                    print(f"[ChatInput] Fetching question from {url}")
                    response = requests.get(url, timeout=10)
                    print(f"[ChatInput] Response status: {response.status_code}")

                    if response.status_code == 200:
                        data = response.json()
                        print(f"[ChatInput] Response data: {data}")
                        text = data.get("question", "")
                        print(f"[ChatInput] Extracted question: '{text}'")

                        if not text:
                            print("[ChatInput] ERROR: 'question' field is empty in response")
                            text = "ERROR: Empty question from server"
                    else:
                        print(f"[ChatInput] ERROR: Server returned status {response.status_code}")
                        print(f"[ChatInput] Response text: {response.text}")
                        text = f"ERROR: Server returned {response.status_code}"

                except requests.exceptions.Timeout:
                    print("[ChatInput] ERROR: Request timeout")
                    text = "ERROR: Timeout fetching question"
                except requests.exceptions.RequestException as e:
                    print(f"[ChatInput] ERROR: Request failed: {e}")
                    text = f"ERROR: Request failed - {e}"
                except Exception as e:
                    print(f"[ChatInput] ERROR: Unexpected error: {e}")
                    import traceback
                    traceback.print_exc()
                    text = f"ERROR: Unexpected error - {e}"
        else:
            print("[ChatInput] Using provided input_value (manual mode)")

        print(f"[ChatInput] Final text: '{text[:100]}...'")

        message = await Message.create(
            text=text,
            sender=self.sender,
            sender_name=self.sender_name,
            session_id=self.session_id,
            files=self.files,
            properties={
                "background_color": background_color,
                "text_color": text_color,
                "icon": icon,
            },
        )
        if (
            self.session_id
            and isinstance(message, Message)
            and self.should_store_message
        ):
            stored_message = await self.send_message(
                message,
            )
            self.message.value = stored_message
            message = stored_message

        self.status = message
        print("[ChatInput] Message created successfully")
        print("=" * 60)
        return message
'''

        node["data"]["node"]["template"]["code"]["value"] = new_code

        # 同时确保 input_value 的默认值为空
        if "input_value" in node["data"]["node"]["template"]:
            node["data"]["node"]["template"]["input_value"]["value"] = ""
            print("[OK] Set input_value default to empty string")

        print("[OK] Added debug logging and error handling to ChatInput")

        # 保存
        with open(json_file, "w", encoding="utf-8") as out_f:
            json.dump(data, out_f, indent=2, ensure_ascii=False)
        print(f"[OK] Saved to {json_file}")
        break

print("\nDone!")
