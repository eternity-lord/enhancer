package com.enhancer.app.util

import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import com.enhancer.app.model.OutputFormat

data class PendingMediaStoreOutput(
    val uri: Uri,
    val detachedFd: Int,
    val fileName: String
)

object MediaStoreSaver {

    fun createPendingOutput(
        context: Context,
        fileName: String,
        format: OutputFormat
    ): PendingMediaStoreOutput? {
        val resolver = context.contentResolver
        val nowSeconds = System.currentTimeMillis() / 1000L

        val values = ContentValues().apply {
            put(MediaStore.Images.Media.DISPLAY_NAME, fileName)
            put(MediaStore.Images.Media.MIME_TYPE, format.mimeType)
            put(MediaStore.Images.Media.DATE_ADDED, nowSeconds)
            put(MediaStore.Images.Media.DATE_MODIFIED, nowSeconds)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                put(
                    MediaStore.Images.Media.RELATIVE_PATH,
                    "${Environment.DIRECTORY_PICTURES}/Enhancer"
                )
                put(MediaStore.Images.Media.IS_PENDING, 1)
            }
        }

        val collectionUri = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            MediaStore.Images.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY)
        } else {
            MediaStore.Images.Media.EXTERNAL_CONTENT_URI
        }

        val itemUri = runCatching {
            resolver.insert(collectionUri, values)
        }.getOrNull() ?: return null

        val pfd = runCatching {
            resolver.openFileDescriptor(itemUri, "w")
        }.getOrNull()

        if (pfd == null) {
            rollbackOutput(context, itemUri)
            return null
        }

        val detachedFd = runCatching { pfd.detachFd() }.getOrElse {
            runCatching { pfd.close() }
            rollbackOutput(context, itemUri)
            return null
        }

        return PendingMediaStoreOutput(
            uri = itemUri,
            detachedFd = detachedFd,
            fileName = fileName
        )
    }

    fun commitPendingOutput(
        context: Context,
        uri: Uri,
        width: Int,
        height: Int
    ) {
        val values = ContentValues().apply {
            if (width > 0) put(MediaStore.Images.Media.WIDTH, width)
            if (height > 0) put(MediaStore.Images.Media.HEIGHT, height)
            put(MediaStore.Images.Media.DATE_MODIFIED, System.currentTimeMillis() / 1000L)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                put(MediaStore.Images.Media.IS_PENDING, 0)
            }
        }
        runCatching {
            context.contentResolver.update(uri, values, null, null)
        }
    }

    fun rollbackOutput(context: Context, uri: Uri?) {
        if (uri == null) return
        runCatching {
            context.contentResolver.delete(uri, null, null)
        }
    }
}
