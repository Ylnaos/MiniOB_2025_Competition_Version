import json
import os
import select
import socket
import subprocess
import time
from typing import Any, Optional, Dict
from typing import List

from langchain_core.documents import Document
from langchain_core.embeddings import Embeddings
from langchain_core.vectorstores import VST, VectorStore
from langflow.base.vectorstores.model import (
    LCVectorStoreComponent,
    check_cached_vector_store,
)
from langflow.base.vectorstores.vector_store_connection_decorator import (
    vector_store_connection,
)
from langflow.helpers import docs_to_data
from langflow.inputs import StrInput, IntInput, FloatInput
from langflow.io import HandleInput
from langflow.schema.data import Data
from langflow.serialization import serialize


class MiniOBException(Exception):
    """Base exception class for MiniOB operations."""

    def __init__(
        self, message: str = None, operation: str = None, details: Dict[str, Any] = None
    ) -> None:
        self.operation = operation
        self.details = details or {}

        if message:
            full_message = message
        else:
            full_message = "MiniOB operation failed"

        if operation:
            full_message = f"[{operation}] {full_message}"

        if details:
            detail_str = ", ".join([f"{k}={v}" for k, v in details.items()])
            full_message = f"{full_message} (Details: {detail_str})"

        super().__init__(full_message)


class MiniOBConnectionException(MiniOBException):
    """Exception raised when connection to MiniOB server fails."""

    def __init__(
        self,
        message: str = None,
        server_address: str = None,
        server_port: int = None,
        server_socket: str = None,
        error_code: str = None,
    ) -> None:
        details = {}
        if server_address:
            details["server_address"] = server_address
        if server_port:
            details["server_port"] = server_port
        if server_socket:
            details["server_socket"] = server_socket
        if error_code:
            details["error_code"] = error_code

        super().__init__(
            message or "Failed to connect to MiniOB server",
            operation="CONNECTION",
            details=details,
        )


class MiniOBQueryException(MiniOBException):
    """Exception raised when SQL query execution fails."""

    def __init__(
        self,
        message: str = None,
        sql: str = None,
        result: str = None,
        execution_time: float = None,
    ) -> None:
        details = {}
        if sql:
            details["sql"] = sql[:200] + "..." if len(sql) > 200 else sql
        if result:
            details["result"] = result
        if execution_time:
            details["execution_time_seconds"] = execution_time

        super().__init__(
            message or "SQL query execution failed",
            operation="QUERY_EXECUTION",
            details=details,
        )


class MiniOBDataException(MiniOBException):
    """Exception raised when data processing fails."""

    def __init__(
        self,
        message: str = None,
        data_type: str = None,
        expected_format: str = None,
        actual_format: str = None,
        row_count: int = None,
    ) -> None:
        details = {}
        if data_type:
            details["data_type"] = data_type
        if expected_format:
            details["expected_format"] = expected_format
        if actual_format:
            details["actual_format"] = actual_format
        if row_count is not None:
            details["row_count"] = row_count

        super().__init__(
            message or "Data processing failed",
            operation="DATA_PROCESSING",
            details=details,
        )


class MiniOBTimeoutException(MiniOBException):
    """Exception raised when operations timeout."""

    def __init__(
        self,
        message: str = None,
        timeout_seconds: float = None,
        operation_type: str = None,
    ) -> None:
        details = {}
        if timeout_seconds:
            details["timeout_seconds"] = timeout_seconds
        if operation_type:
            details["operation_type"] = operation_type

        super().__init__(
            message or "Operation timed out", operation="TIMEOUT", details=details
        )


class MiniObConnector(object):
    def __init__(
        self,
        server_address: str,
        server_port: int,
        server_socket: str,
        time_limit: float,
        charset: str,
        log_func=None,
    ):
        if server_port < 0 or server_port > 65535:
            raise MiniOBConnectionException(
                message=f"Invalid server port: {server_port}",
                server_address=server_address,
                server_port=server_port,
                error_code="INVALID_PORT",
            )

        self.log_func = log_func or (lambda msg: None)

        self.__server_address = server_address
        self.__server_port = server_port
        self.__server_socket = os.getenv("MINIOB_SERVER_SOCKET", "")
        self.__buffer_size = 8192
        self.__charset = charset
        self.__socket = None

        self.__time_limit = time_limit
        if not self.__time_limit:
            self.__time_limit = 10.0

        self.log_func(f"Initializing MiniOB connector...")
        try:
            if len(self.__server_socket) > 0:
                self.log_func(f"Using Unix socket: {self.__server_socket}")
                sock = self.__init_unix_socket(self.__server_socket)
            else:
                self.log_func(f"Using TCP connection: {server_address}:{server_port}")
                sock = self.__init_tcp_socket(self.__server_address, self.__server_port)

            self.__socket = sock
            if sock is not None:
                self.log_func("Socket connection established successfully")
                self.__socket.setblocking(False)

                self.__poller = select.poll()
                self.__poller.register(
                    self.__socket,
                    select.POLLIN | select.POLLPRI | select.POLLHUP | select.POLLERR,
                )
                self.log_func("Socket poller configured successfully")
            else:
                self.log_func("Failed to establish socket connection")
                raise MiniOBConnectionException(
                    message="Socket connection returned None",
                    server_address=server_address,
                    server_port=server_port,
                    server_socket=server_socket,
                    error_code="NULL_SOCKET",
                )
        except Exception as e:
            if isinstance(e, MiniOBConnectionException):
                raise
            else:
                raise MiniOBConnectionException(
                    message=f"Failed to initialize connection: {str(e)}",
                    server_address=server_address,
                    server_port=server_port,
                    server_socket=server_socket,
                    error_code="INIT_FAILED",
                ) from e

    @staticmethod
    def __init_tcp_socket(server_address: str, server_port: int):
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.connect((server_address, server_port))
            return s
        except socket.error as e:
            raise MiniOBConnectionException(
                message=f"Failed to establish TCP connection: {str(e)}",
                server_address=server_address,
                server_port=server_port,
                error_code="TCP_CONNECTION_FAILED",
            ) from e
        except Exception as e:
            raise MiniOBConnectionException(
                message=f"Unexpected error during TCP connection: {str(e)}",
                server_address=server_address,
                server_port=server_port,
                error_code="TCP_UNEXPECTED_ERROR",
            ) from e

    @staticmethod
    def __init_unix_socket(server_socket: str):
        try:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.connect(server_socket)
            return sock
        except socket.error as e:
            raise MiniOBConnectionException(
                message=f"Failed to establish Unix socket connection: {str(e)}",
                server_socket=server_socket,
                error_code="UNIX_SOCKET_CONNECTION_FAILED",
            ) from e
        except Exception as e:
            raise MiniOBConnectionException(
                message=f"Unexpected error during Unix socket connection: {str(e)}",
                server_socket=server_socket,
                error_code="UNIX_SOCKET_UNEXPECTED_ERROR",
            ) from e

    def __recv_response(
        self,
        timeout: float,
        total_timeout_seconds: int,
    ) -> str:
        result = b""

        if not timeout:
            timeout = self.__time_limit

        if timeout is not None:
            timeout *= 1000

        deadline = time.time() + 3600 * 24
        if total_timeout_seconds is not None and total_timeout_seconds > 0:
            deadline = time.time() + total_timeout_seconds

        while time.time() < deadline:
            events = self.__poller.poll(timeout)
            if len(events) == 0:
                raise MiniOBTimeoutException(
                    message=f"Poll timeout after {timeout / 1000} second(s)",
                    timeout_seconds=timeout / 1000,
                    operation_type="SOCKET_POLL",
                )

            (_, event) = events[0]
            if event & (select.POLLHUP | select.POLLERR):
                error_details = {
                    "POLLHUP": bool(event & select.POLLHUP),
                    "POLLERR": bool(event & select.POLLERR),
                    "event_code": event,
                }
                msg = (
                    f"Failed to receive from server. poll return "
                    f"POLLHUP={str(event & select.POLLHUP)} or POLLERR={str(event & select.POLLERR)}"
                )
                self.log_func(msg)
                raise MiniOBConnectionException(
                    message="Socket connection error during polling",
                    error_code="SOCKET_POLL_ERROR",
                )

            data = self.__socket.recv(self.__buffer_size)
            if len(data) > 0:
                try:
                    if data[0] == 0 and len(result) == 0:
                        self.log_func("Error: receive from server \\0 byte")
                        ps_ef = subprocess.run(
                            ["ps", "-ef"],
                            stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE,
                            universal_newlines=True,
                        )

                        self.log_func(f"Process status stdout: {str(ps_ef.stdout)}")
                        self.log_func(f"Process status stderr: {str(ps_ef.stderr)}")
                except BaseException as ex:
                    self.log_func(f"Exception during data processing: {ex}")

                result += data
                self.log_func(
                    f"Received from server (size={len(data)}): {data[:100]}..."
                )

                if data.endswith(b"\0"):

                    result = result[:-1]
                    try:
                        decoded_result = result.decode(encoding=self.__charset)
                        self.log_func(
                            f"Successfully decoded response with length {len(decoded_result)}"
                        )
                        return decoded_result.strip() + "\n"
                    except UnicodeDecodeError as e:
                        self.log_func(f"Unicode decode error: {e}")

                        decoded_result = result.decode(
                            encoding=self.__charset, errors="replace"
                        )
                        self.log_func(
                            f"Decoded with error handling, length: {len(decoded_result)}"
                        )
                        return decoded_result.strip() + "\n"
            else:
                self.log_func(f"Error: receive from server returned zero length data")
                raise MiniOBConnectionException(
                    message="Received zero length data from server - connection may be closed",
                    error_code="ZERO_LENGTH_DATA",
                )

        if time.time() >= deadline:
            self.log_func(
                f"Timeout: receive from server timeout after {total_timeout_seconds} seconds"
            )

        raise MiniOBTimeoutException(
            message=f"Response timeout after {total_timeout_seconds} second(s)",
            timeout_seconds=total_timeout_seconds,
            operation_type="RECEIVE_RESPONSE",
        )

    def exec(
        self,
        sql: str,
        timeout=None,
        total_timeout_seconds=None,
    ) -> str:
        if not sql:
            raise MiniOBQueryException(
                message="SQL query cannot be empty or None", sql=sql or "None"
            )

        start_time = time.time()
        self.log_func(f"Executing SQL: {sql}")

        try:
            data = str.encode(sql, self.__charset)
            self.__socket.sendall(data)
            self.__socket.sendall(b"\0")
            self.log_func(f"SQL command sent to server (size={len(data) + 1}): '{sql}'")
            self.log_func("SQL command sent to server, waiting for response...")

            result = self.__recv_response(timeout, total_timeout_seconds)
            execution_time = time.time() - start_time

            self.log_func(f"SQL response received from server: '{result}'")
            self.log_func(f"SQL execution completed with result: {result}")

            result_lower = result.lower().strip()
            if (
                result_lower.startswith("error")
                or "failed" in result_lower
                or "exception" in result_lower
            ):
                raise MiniOBQueryException(
                    message="SQL execution returned error result",
                    sql=sql,
                    result=result,
                    execution_time=execution_time,
                )

            return result.strip("\n")

        except (MiniOBConnectionException, MiniOBTimeoutException):

            raise
        except Exception as e:
            execution_time = time.time() - start_time
            if isinstance(e, MiniOBQueryException):
                raise
            else:
                raise MiniOBQueryException(
                    message=f"Unexpected error during SQL execution: {str(e)}",
                    sql=sql,
                    execution_time=execution_time,
                ) from e

    def close(self):
        if self.__socket:
            self.log_func("Closing MiniOB connector socket...")
            self.__socket.close()
            self.__socket = None
            self.log_func("MiniOB connector socket closed successfully")
        else:
            self.log_func("No socket to close")


class MiniOBVectorStore(VectorStore):
    def __init__(
        self,
        embedding: Embeddings | None,
        server_address: str,
        server_port: int,
        server_socket: str,
        time_limit: float,
        charset: str,
        log_func=None,
    ) -> None:
        self.__embedding: Embeddings | None = embedding
        self.__log_func = log_func or (lambda msg: None)

        self.connector: MiniObConnector = MiniObConnector(
            server_address=server_address,
            server_port=server_port,
            server_socket=server_socket,
            time_limit=time_limit,
            charset=charset,
            log_func=self.__log_func,
        )

        if not self.__embedding:
            raise MiniOBException(
                message="Embedding model is required but was not provided",
                operation="INITIALIZATION",
            )

        try:
            self.__embedding_dimension: int = len(self.__embedding.embed_query("Hello"))
        except Exception as e:
            raise MiniOBException(
                message=f"Failed to determine embedding dimension: {str(e)}",
                operation="EMBEDDING_DIMENSION_CHECK",
                details={"embedding_model": type(self.__embedding).__name__},
            ) from e

        # 初始化元数据：建表与向量索引（若未存在）
        self.__table = "rag_docs"
        self.__index = "idx_rag_docs_embedding"
        self.__ensure_initialized()

    # ------------------------------
    # 内部工具方法
    # ------------------------------
    def __vector_literal(self, vec: List[float]) -> str:
        # 以 JSON 风格输出为 SQL 向量字面量: [v1,v2,...]
        # 保留 8 位小数，足够用于距离排序
        return "[" + ",".join(f"{float(x):.8f}" for x in vec) + "]"

    def __escape_text(self, text: str, limit: int = 3500) -> str:
        # 生成单行、可安全插入 SQL 的 TEXT：
        if text is None:
            text = ""
        # 替换可能影响解析的分隔符与换行
        s = text.replace("|", " ").replace("\r", " ").replace("\n", " ")
        # 压缩多空格
        s = " ".join(s.split())
        # 截断以避免超长
        if limit and len(s) > limit:
            s = s[:limit]
        # 转义单引号
        s = s.replace("'", "''")
        return s

    def __ensure_initialized(self) -> None:
        # 检查表是否存在：若不存在则创建；随后创建/确保存在向量索引
        try:
            _ = self.connector.exec(f"desc {self.__table}")
        except Exception:
            # 建表：仅需 2 列，TEXT 与 VECTOR(dim)
            create_sql = (
                f"create table {self.__table} ("
                f"content text, "
                f"embedding vector({self.__embedding_dimension})"
                f")"
            )
            self.__log_func(f"Creating table: {create_sql}")
            res = self.connector.exec(create_sql)
            self.__log_func(f"Create table result: {res}")

        # 创建向量索引（若未存在）
        try:
            index_sql = (
                f"create vector index if not exists {self.__index} "
                f"on {self.__table} (embedding) "
                f"with (type = ivfflat, distance = cosine_distance, lists = 64, probes = 8)"
            )
            self.__log_func(f"Ensuring vector index: {index_sql}")
            _ = self.connector.exec(index_sql)
        except Exception as e:
            # 索引已存在或不支持时，跳过但记录
            self.__log_func(f"Create vector index ignored: {e}")

    def similarity_search(
        self,
        query: str,
        k: int = 4,
        search_method: str = "Vector Search",
    ) -> list[Document]:
        self.__log_func(f"Performing similarity search for query: '{query}' with k={k}")
        result_docs: list[Document] = []

        if not query:
            return result_docs

        try:
            qvec = self.__embedding.embed_query(query)
        except Exception as e:
            raise MiniOBException(
                message=f"Failed to embed query: {e}",
                operation="EMBED_QUERY",
            ) from e

        vec_lit = self.__vector_literal(qvec)
        # 说明：PlainCommunicator 的查询结果以第一行表头，其后每行一条记录，\n 分隔
        # 为避免文本内换行破坏解析，插入前已做单行化处理
        sql = (
            f"select content from {self.__table} "
            f"order by distance(embedding, {vec_lit}, 'COSINE') limit {int(k)}"
        )
        raw = self.connector.exec(sql)

        lines = [ln for ln in raw.splitlines() if ln and not ln.startswith('#')]
        if not lines:
            return result_docs
        # 第 1 行是列名
        header = [h.strip() for h in lines[0].split(' | ')]
        try:
            cidx = header.index('content')
        except ValueError:
            cidx = 0

        for ln in lines[1:]:
            # 只有 1 列时不含分隔符
            if ' | ' in ln:
                parts = ln.split(' | ')
                content = parts[cidx]
            else:
                content = ln
            result_docs.append(Document(page_content=content, metadata={}))

        self.__log_func(f"Similarity search completed. Found {len(result_docs)} documents")
        return result_docs

    def add_documents(
        self,
        documents: list[Document],
        **kwargs: Any,
    ) -> List[str]:
        if not documents or len(documents) == 0:
            self.__log_func("No documents provided for insertion")
            return []

        self.__log_func(f"Processing {len(documents)} documents for insertion")
        page_contents: List[str] = [document.page_content for document in documents]
        self.__log_func("Generating embeddings for documents...")
        try:
            embeddings: List[List[float]] = self.__embedding.embed_documents(
                page_contents
            )
        except Exception as e:
            raise MiniOBException(
                message=f"Failed to generate embeddings for documents: {str(e)}",
                operation="GENERATE_EMBEDDINGS",
                details={"document_count": len(page_contents)},
            ) from e

        if len(embeddings) != len(page_contents):
            self.__log_func(
                f"Error: Embedding count mismatch. Expected {len(page_contents)}, got {len(embeddings)}"
            )
            raise MiniOBDataException(
                message="Embedding count mismatch with document count",
                data_type="embeddings",
                expected_format=f"{len(page_contents)} embeddings",
                actual_format=f"{len(embeddings)} embeddings",
                row_count=len(page_contents),
            )

        # 批量写入，避免超过 4MB 包大小，分批 50 行
        batch_size = 50
        total = len(page_contents)
        inserted = 0
        for start in range(0, total, batch_size):
            values_sql = []
            for i in range(start, min(start + batch_size, total)):
                text = self.__escape_text(page_contents[i])
                vec_lit = self.__vector_literal(embeddings[i])
                values_sql.append(f"('{text}', {vec_lit})")

            if not values_sql:
                continue

            insert_sql = (
                f"insert into {self.__table} (content, embedding) values "
                + ",".join(values_sql)
            )
            res = self.connector.exec(insert_sql)
            self.__log_func(f"Inserted {len(values_sql)} rows. Result: {res}")
            inserted += len(values_sql)

        # 可选：触发表统计优化
        try:
            _ = self.connector.exec(f"analyze table {self.__table}")
        except Exception:
            pass

        return [str(i) for i in range(inserted)]

    # Langflow 组件适配：component 调用的是 vector_store.search(...)
    def search(self, query: str, search_type: str = "similarity", k: int = 4):
        return self.similarity_search(query=query, k=k)

    @classmethod
    def from_texts(
        cls: type[VST],
        texts: list[str],
        embedding: Embeddings,
        metadatas: Optional[list[dict]] = None,
        *,
        ids: Optional[list[str]] = None,
        **kwargs: Any,
    ) -> VST:
        raise NotImplementedError()


@vector_store_connection
class MiniOBVectorStoreComponent(LCVectorStoreComponent):
    display_name = "MiniOB"
    description = "miniob vector store component."
    documentation: str = "https://oceanbase.github.io/miniob/"
    icon = "database"
    name = "MiniOB"

    _cached_vector_store: MiniOBVectorStore | None = None

    inputs = [
        StrInput(
            name="server_address",
            display_name="Server Address",
            advanced=True,
            value="127.0.0.1",
        ),
        IntInput(
            name="server_port",
            display_name="Server port",
            advanced=True,
            value=6789,
        ),
        StrInput(
            name="server_socket",
            display_name="Server socket",
            advanced=True,
        ),
        IntInput(
            name="number_of_results",
            display_name="Number of Search Results",
            advanced=True,
            value=4,
        ),
        FloatInput(
            name="time_limit",
            display_name="Search Time Limit",
            advanced=True,
            value=10.0,
        ),
        StrInput(
            name="charset",
            display_name="Charset",
            advanced=True,
            value="utf-8",
        ),
        HandleInput(
            name="embedding_model",
            display_name="Embedding Model",
            input_types=["Embeddings"],
            info="",
            required=True,
            show=True,
        ),
        *LCVectorStoreComponent.inputs,
    ]

    outputs = [
        *LCVectorStoreComponent.outputs,
    ]
    
    def _already_build(self) -> bool:
        # 判断是否已构建：表存在且行数>0 即视为已构建
        try:
            # 触发连接与表/索引检查
            vs = MiniOBVectorStore(
                embedding=self.embedding_model if self.embedding_model else None,
                server_address=self.server_address if self.server_address else "127.0.0.1",
                server_port=self.server_port if self.server_port else 6789,
                server_socket=self.server_socket if self.server_socket else "",
                time_limit=self.time_limit if self.time_limit else 10.0,
                charset=self.charset if self.charset else "utf-8",
                log_func=self.log,
            )
            raw = vs.connector.exec("select count(*) from rag_docs limit 1")
            # 解析结果：第一行表头，第二行数值
            lines = [ln for ln in raw.splitlines() if ln and not ln.startswith('#')]
            if len(lines) >= 2:
                try:
                    cnt = int(lines[1].split(' | ')[-1].strip())
                    return cnt > 0
                except Exception:
                    return False
            return False
        except Exception:
            return False

    def _add_documents_to_vector_store(self, vector_store: MiniOBVectorStore) -> None:
        self.log("Preparing to add documents to vector store...")
        self.ingest_data = self._prepare_ingest_data()

        documents = []
        for _input in self.ingest_data or []:
            if isinstance(_input, Data):
                documents.append(_input.to_lc_document())
            else:
                msg = "Vector Store Inputs must be Data objects."
                self.log(
                    f"Error: Invalid input type {type(_input)}, expected Data object"
                )
                raise TypeError(msg)

        self.log(f"Prepared {len(documents)} documents for ingestion")

        documents = [
            Document(
                page_content=doc.page_content,
                metadata=serialize(doc.metadata, to_str=True),
            )
            for doc in documents
        ]

        if documents:
            self.log(f"Adding {len(documents)} documents to the Vector Store.")
            try:
                vector_store.add_documents(documents)
                self.log(
                    f"Successfully added {len(documents)} documents to Vector Store"
                )
            except Exception as e:
                self.log(f"Error adding documents to Vector Store: {str(e)}")
                raise
        else:
            self.log("No documents to add to the Vector Store.")
            return None

    @check_cached_vector_store
    def build_vector_store(self) -> VectorStore:
        self.log("Building MiniOB Vector Store...")
        self.log(f"Connecting to server: {self.server_address}:{self.server_port}")

        self.log(
            f"Configuration: server_address={self.server_address}, server_port={self.server_port}"
        )
        self.log(
            f"Configuration: server_socket={self.server_socket}, time_limit={self.time_limit}"
        )
        self.log(
            f"Configuration: charset={self.charset}, embedding_model={type(self.embedding_model).__name__ if self.embedding_model else 'None'}"
        )

        vector_store = MiniOBVectorStore(
            embedding=self.embedding_model if self.embedding_model else None,
            server_address=self.server_address if self.server_address else "127.0.0.1",
            server_port=self.server_port if self.server_port else 6789,
            server_socket=self.server_socket if self.server_socket else "",
            time_limit=self.time_limit if self.time_limit else 10.0,
            charset=self.charset if self.charset else "utf-8",
            log_func=self.log,
        )

        self.log("Vector Store created successfully")
        if not self._already_build():
            self._add_documents_to_vector_store(vector_store)
        self.log("Vector Store build completed")
        return vector_store

    def search_documents(self, vector_store=None) -> list[Data]:
        if not vector_store:
            vector_store = self.build_vector_store()

        query = (self.search_query if self.search_query else "").strip()
        if not query:
            self.log("Warning: Empty search query provided")
            return []

        self.log(f"Searching for documents with query: '{query}'")
        self.log(
            f"Number of results requested: {self.number_of_results if self.number_of_results else 4}"
        )

        vector_store = vector_store or self.build_vector_store()

        data = docs_to_data(
            vector_store.search(
                query=query,
                search_type="similarity",
                k=self.number_of_results if self.number_of_results else 4,
            )
        )

        if not data:
            self.log(f"No documents found for query: '{query}'")
            return []

        self.log(f"Found {len(data)} documents for query: '{query}'")
        return data
