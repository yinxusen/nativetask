/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package org.apache.hadoop.mapred.nativetask.serde;

import java.io.ByteArrayOutputStream;
import java.io.DataInput;
import java.io.DataOutput;
import java.io.DataOutputStream;
import java.io.IOException;

import org.apache.hadoop.io.Writable;

public class DefaultSerializer implements INativeSerializer<Writable> {

  static class ModifiedByteArrayOutputStream extends ByteArrayOutputStream {

    public byte[] getBuffer() {
      return this.buf;
    }
  }

  private ModifiedByteArrayOutputStream outBuffer = new ModifiedByteArrayOutputStream();
  private DataOutputStream outData = new DataOutputStream(outBuffer);
  private Writable buffered = null;
  private int bufferedLength = -1;

  @Override
  public int getLength(Writable w) throws IOException {
    // if (w == buffered) {
    // return bufferedLength;
    // }
    buffered = null;
    bufferedLength = -1;

    outBuffer.reset();
    w.write(outData);
    bufferedLength = outBuffer.size();
    buffered = w;
    return bufferedLength;
  }

  @Override
  public void serialize(Writable w, DataOutput out) throws IOException {
    // if (w == buffered) {
    // out.write(outBuffer.getBuffer(), 0, outBuffer.size());
    // return;
    // }
    w.write(out);
  }

  @Override
  public void deserialize(Writable w, int length, DataInput in)
      throws IOException {
    w.readFields(in);
  }
}
